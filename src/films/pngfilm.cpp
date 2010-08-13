#include <mitsuba/render/film.h>
#include <mitsuba/core/bitmap.h>
#include <mitsuba/core/fresolver.h>
#include "banner.h"

MTS_NAMESPACE_BEGIN

/**
 * Simple film implementation, which stores the captured image
 * as an RGBA-based low dynamic-range PNG file with gamma correction.
 * Spectral radiance values are converted to linear RGB using 
 * the CIE 1931 XYZ color matching functions and ITU-R Rec. BT.709
 */
class PNGFilm : public Film {
protected:
	/* Pixel data structure */
	struct Pixel {
		Spectrum spec;
		Float alpha;
		Float weight;

		Pixel() : spec(0.0f), alpha(0.0f), weight(0.0f) {
		}
	};

	Pixel *m_pixels;
	int m_bpp;
	bool m_hasBanner;
	bool m_hasAlpha;
	Float m_gamma;
public:
	PNGFilm(const Properties &props) : Film(props) {
		m_pixels = new Pixel[m_cropSize.x * m_cropSize.y];
		/* Should an alpha channel be added to the output image? */
		m_hasAlpha = props.getBoolean("alpha", true);
		/* Should an Mitsuba banner be added to the output image? */
		m_hasBanner = props.getBoolean("banner", true);
		/* Bits per pixel including alpha (must be 8, 16, 24 or 32) */
		m_bpp = props.getInteger("bpp", -1);
		/* Gamma value for the correction. Negative values switch to sRGB */
		m_gamma = props.getFloat("gamma", -1.0); // -1: sRGB. 

		if (m_bpp == -1)
			m_bpp = m_hasAlpha ? 32 : 24;
		if (m_bpp == 24 && m_hasAlpha)
			Log(EError, "24bpp implies *no* alpha channel!");
		if (m_bpp == 8 && m_hasAlpha)
			Log(EError, "8bpp implies *no* alpha channel!");
		if (m_bpp == 32 && !m_hasAlpha)
			Log(EError, "32bpp implies an alpha channel!");
		if (m_bpp == 16 && !m_hasAlpha)
			Log(EError, "16bpp implies a grayscale image with an alpha channel!");
		if (m_bpp != 8 && m_bpp != 16 && m_bpp != 24 && m_bpp != 32)
			Log(EError, "The PNG film must be set to 8, 16, 24 or 32 bits per pixel!");
	}

	PNGFilm(Stream *stream, InstanceManager *manager) 
		: Film(stream, manager) {
		m_hasAlpha = stream->readBool();
		m_bpp = stream->readInt();
		m_gamma = stream->readFloat();
		m_gamma = 1.0f / m_gamma;
		m_pixels = new Pixel[m_cropSize.x * m_cropSize.y];
	}

	void serialize(Stream *stream, InstanceManager *manager) const {
		Film::serialize(stream, manager);

		stream->writeBool(m_hasAlpha);
		stream->writeInt(m_bpp);
		stream->writeFloat(m_gamma);
	}

	virtual ~PNGFilm() {
		if (m_pixels)
			delete[] m_pixels;
	}

	void clear() {
		memset(m_pixels, 0, sizeof(Pixel) * m_cropSize.x * m_cropSize.y);
	}

	void fromBitmap(const Bitmap *bitmap) {
		Assert(bitmap->getWidth() == m_cropSize.x 
			&& bitmap->getHeight() == m_cropSize.y);
		Assert(bitmap->getBitsPerPixel() == 128);
		unsigned int lastIndex = m_cropSize.x * m_cropSize.y;

		for (unsigned int index=0; index<lastIndex; ++index) {
			Pixel &pixel = m_pixels[index];
			const float 
				r = bitmap->getFloatData()[index*4+0],
				g = bitmap->getFloatData()[index*4+1],
				b = bitmap->getFloatData()[index*4+2],
				a = bitmap->getFloatData()[index*4+3];
			pixel.spec.fromLinearRGB(r, g, b);
			pixel.alpha = a;
			pixel.weight = 1.0f;
		}
	}

	void toBitmap(Bitmap *bitmap) const {
		Assert(bitmap->getWidth() == m_cropSize.x 
			&& bitmap->getHeight() == m_cropSize.y);
		Assert(bitmap->getBitsPerPixel() == 128);
		unsigned int lastIndex = m_cropSize.x * m_cropSize.y;
		Float r, g, b, a;

		for (unsigned int index=0; index<lastIndex; ++index) {
			Pixel &pixel = m_pixels[index];
			Float invWeight = pixel.weight > 0 ? 1/pixel.weight : 0;
			pixel.spec.toLinearRGB(r, g, b);
			a = pixel.alpha;
			bitmap->getFloatData()[index*4+0] = r*invWeight;
			bitmap->getFloatData()[index*4+1] = g*invWeight;
			bitmap->getFloatData()[index*4+2] = b*invWeight;
			bitmap->getFloatData()[index*4+3] = a*invWeight;
		}
	}

	Spectrum getValue(int xPixel, int yPixel) {
		xPixel -= m_cropOffset.x; yPixel -= m_cropOffset.y;
		if (!(xPixel >= 0 && xPixel < m_cropSize.x && yPixel >= 0 && yPixel < m_cropSize.y)) {
			Log(EWarn, "Pixel out of range : %i,%i", xPixel, yPixel); 
			return Spectrum(0.0f);
		}
		Pixel &pixel = m_pixels[xPixel + yPixel * m_cropSize.x];
		return pixel.spec / pixel.weight;
	}

	void putImageBlock(const ImageBlock *block) {
		int entry=0, imageY = block->getOffset().y - 
			block->getBorder() - m_cropOffset.y - 1;

		for (int y=0; y<block->getFullSize().y; ++y) {
			if (++imageY < 0 || imageY >= m_cropSize.y) {
				/// Skip a row if it is outside of the crop region
				entry += block->getFullSize().x;
				continue;
			}

			int imageX = block->getOffset().x - block->getBorder()
				- m_cropOffset.x - 1;
			for (int x=0; x<block->getFullSize().x; ++x) {
				if (++imageX < 0 || imageX >= m_cropSize.x) {
					++entry;
					continue;
				}

				Pixel &pixel = m_pixels[imageY * m_cropSize.x + imageX];

				pixel.spec += block->getPixel(entry);
				pixel.alpha += block->getAlpha(entry);
				pixel.weight += block->getWeight(entry++);
			}
		}
	}
	inline Float toSRGBComponent(Float value) {
		if (value <= (Float) 0.0031308)
			return (Float) 12.92 * value;
		return (Float) (1.0 + 0.055)
			* std::pow(value, (Float) (1.0/2.4))
			- (Float) 0.055;
	}

	void develop(const std::string &destFile) {
		Log(EDebug, "Developing film ..");
		ref<Bitmap> bitmap = new Bitmap(m_cropSize.x, m_cropSize.y, m_bpp);
		uint8_t *targetPixels = bitmap->getData();
		Float r, g, b;
		int pos = 0;

		if (m_bpp == 32) {
			for (int y=0; y<m_cropSize.y; y++) {
				for (int x=0; x<m_cropSize.x; x++) {
					/* Convert spectrum to sRGB */
					Pixel &pixel = m_pixels[pos];
					Float invWeight = 1.0f;
					if (pixel.weight != 0.0f)
						invWeight = 1.0f / pixel.weight;
					if (m_gamma < 0)
						(pixel.spec * invWeight).toSRGB(r, g, b);
					else
						(pixel.spec * invWeight).pow(m_gamma).toLinearRGB(r, g, b);

					targetPixels[4*pos+0] = (uint8_t) clamp((int) (r * 255), 0, 255);
					targetPixels[4*pos+1] = (uint8_t) clamp((int) (g * 255), 0, 255);
					targetPixels[4*pos+2] = (uint8_t) clamp((int) (b * 255), 0, 255);
					targetPixels[4*pos+3] = (uint8_t) (m_hasAlpha ? 
						clamp((int) (pixel.alpha*invWeight*255), 0, 255) : 255);
					++pos;
				}
			}

			if (m_hasBanner && m_cropSize.x > bannerWidth+5 && m_cropSize.y > bannerHeight + 5) {
				int xoffs = m_cropSize.x - bannerWidth - 5, yoffs = m_cropSize.y - bannerHeight - 5;
				for (int y=0; y<bannerHeight; y++) {
					for (int x=0; x<bannerWidth; x++) {
						int value = (1-banner[x+y*bannerWidth])*255;
						int pos = 4*((x+xoffs)+(y+yoffs)*m_cropSize.x);
						targetPixels[pos+0] = (uint8_t) clamp(value + targetPixels[pos+0], 0, 255);
						targetPixels[pos+1] = (uint8_t) clamp(value + targetPixels[pos+1], 0, 255);
						targetPixels[pos+2] = (uint8_t) clamp(value + targetPixels[pos+2], 0, 255);
						targetPixels[pos+3] = (uint8_t) 255;
					}
				}
			}
		} else if (m_bpp == 24) {
			for (int y=0; y<m_cropSize.y; y++) {
				for (int x=0; x<m_cropSize.x; x++) {
					/* Convert spectrum to sRGB */
					Pixel &pixel = m_pixels[pos];
					Float invWeight = 1.0f;
					if (pixel.weight != 0.0f)
						invWeight = 1.0f / pixel.weight;
					if (m_gamma < 0)
						(pixel.spec * invWeight).toSRGB(r, g, b);
					else
						(pixel.spec * invWeight).pow(m_gamma).toLinearRGB(r, g, b);

					targetPixels[3*pos+0] = (uint8_t) clamp((int) (r * 255), 0, 255);
					targetPixels[3*pos+1] = (uint8_t) clamp((int) (g * 255), 0, 255);
					targetPixels[3*pos+2] = (uint8_t) clamp((int) (b * 255), 0, 255);
					++pos;
				}
			}

			if (m_hasBanner && m_cropSize.x > bannerWidth+5 && m_cropSize.y > bannerHeight + 5) {
				int xoffs = m_cropSize.x - bannerWidth - 5, yoffs = m_cropSize.y - bannerHeight - 5;
				for (int y=0; y<bannerHeight; y++) {
					for (int x=0; x<bannerWidth; x++) {
						int value = (1-banner[x+y*bannerWidth])*255;
						int pos = 3*((x+xoffs)+(y+yoffs)*m_cropSize.x);
						targetPixels[pos+0] = (uint8_t) clamp(value + targetPixels[pos+0], 0, 255);
						targetPixels[pos+1] = (uint8_t) clamp(value + targetPixels[pos+1], 0, 255);
						targetPixels[pos+2] = (uint8_t) clamp(value + targetPixels[pos+2], 0, 255);
					}
				}
			}
		} else if (m_bpp == 16) {
			for (int y=0; y<m_cropSize.y; y++) {
				for (int x=0; x<m_cropSize.x; x++) {
					/* Convert spectrum to sRGB */
					Pixel &pixel = m_pixels[pos];
					Float invWeight = 1.0f;
					if (pixel.weight != 0.0f)
						invWeight = 1.0f / pixel.weight;
					Float luminance = (pixel.spec * invWeight).getLuminance();
					if (m_gamma < 0)
						luminance = toSRGBComponent(luminance);
					else
						luminance = std::pow(luminance, m_gamma);
					targetPixels[2*pos+0] = (uint8_t) clamp((int) (luminance * 255), 0, 255);
					targetPixels[2*pos+1] = (uint8_t) (m_hasAlpha ? 
						clamp((int) (pixel.alpha*invWeight*255), 0, 255) : 255);
					++pos;
				}
			}

			if (m_hasBanner && m_cropSize.x > bannerWidth+5 && m_cropSize.y > bannerHeight + 5) {
				int xoffs = m_cropSize.x - bannerWidth - 5, yoffs = m_cropSize.y - bannerHeight - 5;
				for (int y=0; y<bannerHeight; y++) {
					for (int x=0; x<bannerWidth; x++) {
						int value = 2*(1-banner[x+y*bannerWidth])*255;
						int pos = 2*((x+xoffs)+(y+yoffs)*m_cropSize.x);
						targetPixels[pos+0] = (uint8_t) clamp(value + targetPixels[pos], 0, 255);
						targetPixels[pos+1] = (uint8_t) 255;
					}
				}
			}
		} else if (m_bpp == 8) {
			for (int y=0; y<m_cropSize.y; y++) {
				for (int x=0; x<m_cropSize.x; x++) {
					/* Convert spectrum to sRGB */
					Pixel &pixel = m_pixels[pos];
					Float invWeight = 1.0f;
					if (pixel.weight != 0.0f)
						invWeight = 1.0f / pixel.weight;
					Float luminance = (pixel.spec * invWeight).getLuminance();
					if (m_gamma < 0)
						luminance = toSRGBComponent(luminance);
					else
						luminance = std::pow(luminance, m_gamma);
					targetPixels[pos++] = (uint8_t) clamp((int) (luminance * 255), 0, 255);
				}
			}

			if (m_hasBanner && m_cropSize.x > bannerWidth+5 && m_cropSize.y > bannerHeight + 5) {
				int xoffs = m_cropSize.x - bannerWidth - 5, yoffs = m_cropSize.y - bannerHeight - 5;
				for (int y=0; y<bannerHeight; y++) {
					for (int x=0; x<bannerWidth; x++) {
						int value = (1-banner[x+y*bannerWidth])*255;
						int pos = (x+xoffs)+(y+yoffs)*m_cropSize.x;
						targetPixels[pos] = (uint8_t) clamp(value + targetPixels[pos], 0, 255);
					}
				}
			}
		}

		std::string filename = destFile;
		if (!endsWith(filename, ".png"))
			filename += ".png";

		Log(EInfo, "Writing image to \"%s\" ..", filename.c_str());
		ref<FileStream> stream = new FileStream(filename, FileStream::ETruncWrite);
		bitmap->setGamma(m_gamma);
		bitmap->save(Bitmap::EPNG, stream);
	}

	bool destinationExists(const std::string &baseName) const {
		std::string filename = baseName;
		if (!endsWith(filename, ".png"))
			filename += ".png";
		return FileStream::exists(filename);
	}

	std::string toString() const {
		std::ostringstream oss;
		oss << "PNGFilm[" << std::endl
			<< "  size = " << m_size.toString() << "," << std::endl
			<< "  cropOffset = " << m_cropOffset.toString() << "," << std::endl
			<< "  cropSize = " << m_cropSize.toString() << "," << std::endl
			<< "  bpp = " << m_bpp << "," << std::endl
			<< "  gamma = " << m_gamma << "," << std::endl
			<< "  banner = " << m_hasBanner << std::endl
			<< "]";
		return oss.str();
	}

	MTS_DECLARE_CLASS()
};

MTS_IMPLEMENT_CLASS_S(PNGFilm, false, Film)
MTS_EXPORT_PLUGIN(PNGFilm, "Low dynamic-range film (PNG)");
MTS_NAMESPACE_END