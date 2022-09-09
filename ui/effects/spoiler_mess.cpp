// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/effects/spoiler_mess.h"

#include "ui/painter.h"
#include "ui/integration.h"
#include "base/random.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QDir>

#include <crl/crl_async.h>
#include <xxhash.h>

namespace Ui {
namespace {

constexpr auto kVersion = 1;
constexpr auto kFramesPerRow = 10;
constexpr auto kImageSpoilerDarkenAlpha = 32;
constexpr auto kMaxCacheSize = 5 * 1024 * 1024;
constexpr auto kDefaultFrameDuration = crl::time(33);
constexpr auto kDefaultFramesCount = 60;
constexpr auto kDefaultCanvasSize = 100;

std::atomic<const SpoilerMessCached*> DefaultMask/* = nullptr*/;
std::condition_variable *DefaultMaskSignal/* = nullptr*/;
std::mutex *DefaultMaskMutex/* = nullptr*/;

struct Header {
	uint32 version = 0;
	uint32 dataLength = 0;
	uint32 dataHash = 0;
	int32 framesCount = 0;
	int32 canvasSize = 0;
	int32 frameDuration = 0;
};

struct Particle {
	crl::time start = 0;
	int spriteIndex = 0;
	int x = 0;
	int y = 0;
};

[[nodiscard]] Particle GenerateParticle(
		const SpoilerMessDescriptor &descriptor,
		int index,
		base::BufferedRandom<uint32> &random) {
	return {
		.start = (index * descriptor.framesCount * descriptor.frameDuration
			/ descriptor.particlesCount),
		.spriteIndex = RandomIndex(descriptor.particleSpritesCount, random),
		.x = RandomIndex(descriptor.canvasSize, random),
		.y = RandomIndex(descriptor.canvasSize, random),
	};
}

[[nodiscard]] QImage GenerateSprite(
		const SpoilerMessDescriptor &descriptor,
		int index,
		int size,
		base::BufferedRandom<uint32> &random) {
	Expects(index >= 0 && index < descriptor.particleSpritesCount);

	const auto count = descriptor.particleSpritesCount;
	const auto middle = count / 2;
	const auto min = descriptor.particleSizeMin;
	const auto delta = descriptor.particleSizeMax - min;
	const auto width = (index < middle)
		? (min + delta * (middle - index) / float64(middle))
		: min;
	const auto height = (index > middle)
		? (min + delta * (index - middle) / float64(count - 1 - middle))
		: min;
	const auto radius = min / 2.;

	auto result = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawRoundedRect(1., 1., width, height, radius, radius);
	p.end();
	return result;
}

[[nodiscard]] QString DefaultMaskCacheFolder() {
	const auto base = Integration::Instance().emojiCacheFolder();
	return base.isEmpty() ? QString() : (base + "/spoiler");
}

[[nodiscard]] QString DefaultMaskCachePath(const QString &folder) {
	return folder + "/mask";
}

[[nodiscard]] std::optional<SpoilerMessCached> ReadDefaultMask(
		std::optional<SpoilerMessCached::Validator> validator) {
	const auto folder = DefaultMaskCacheFolder();
	if (folder.isEmpty()) {
		return {};
	}
	auto file = QFile(DefaultMaskCachePath(folder));
	return (file.open(QIODevice::ReadOnly) && file.size() <= kMaxCacheSize)
		? SpoilerMessCached::FromSerialized(file.readAll(), validator)
		: std::nullopt;
}

void WriteDefaultMask(const SpoilerMessCached &mask) {
	const auto folder = DefaultMaskCacheFolder();
	if (!QDir().mkpath(folder)) {
		return;
	}
	const auto bytes = mask.serialize();
	auto file = QFile(DefaultMaskCachePath(folder));
	if (file.open(QIODevice::WriteOnly) && bytes.size() <= kMaxCacheSize) {
		file.write(bytes);
	}
}

} // namespace

SpoilerMessCached GenerateSpoilerMess(
		const SpoilerMessDescriptor &descriptor) {
	Expects(descriptor.framesCount > 0);
	Expects(descriptor.frameDuration > 0);
	Expects(descriptor.particlesCount > 0);
	Expects(descriptor.canvasSize > 0);
	Expects(descriptor.particleSizeMax >= descriptor.particleSizeMin);
	Expects(descriptor.particleSizeMin > 0.);

	const auto frames = descriptor.framesCount;
	const auto rows = (frames + kFramesPerRow - 1) / kFramesPerRow;
	const auto columns = std::min(frames, kFramesPerRow);
	const auto size = descriptor.canvasSize;
	const auto count = descriptor.particlesCount;
	const auto width = size * columns;
	const auto height = size * rows;
	const auto spriteSize = 2 + int(std::ceil(descriptor.particleSizeMax));
	const auto singleDuration = descriptor.particleFadeInDuration
		+ descriptor.particleShownDuration
		+ descriptor.particleFadeOutDuration;
	const auto fullDuration = frames * descriptor.frameDuration;
	Assert(fullDuration > singleDuration);

	auto random = base::BufferedRandom<uint32>(count * 3);

	auto particles = std::vector<Particle>();
	particles.reserve(descriptor.particlesCount);
	for (auto i = 0; i != descriptor.particlesCount; ++i) {
		particles.push_back(GenerateParticle(descriptor, i, random));
	}

	auto sprites = std::vector<QImage>();
	sprites.reserve(descriptor.particleSpritesCount);
	for (auto i = 0; i != descriptor.particleSpritesCount; ++i) {
		sprites.push_back(GenerateSprite(descriptor, i, spriteSize, random));
	}

	auto frame = 0;
	auto image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto p = QPainter(&image);
	const auto paintOneAt = [&](const Particle &particle, crl::time time) {
		if (time <= 0 || time >= singleDuration) {
			return;
		}
		const auto opacity = (time < descriptor.particleFadeInDuration)
			? (time / float64(descriptor.particleFadeInDuration))
			: (time > singleDuration - descriptor.particleFadeOutDuration)
			? ((singleDuration - time)
				/ float64(descriptor.particleFadeOutDuration))
			: 1.;
		p.setOpacity(opacity);
		const auto &sprite = sprites[particle.spriteIndex];
		p.drawImage(particle.x, particle.y, sprite);
		if (particle.x + spriteSize > size) {
			p.drawImage(particle.x - size, particle.y, sprite);
			if (particle.y + spriteSize > size) {
				p.drawImage(particle.x, particle.y - size, sprite);
				p.drawImage(particle.x - size, particle.y - size, sprite);
			}
		} else if (particle.y + spriteSize > size) {
			p.drawImage(particle.x, particle.y - size, sprite);
		}
	};
	const auto paintOne = [&](const Particle &particle, crl::time now) {
		paintOneAt(particle, now - particle.start);
		paintOneAt(particle, now + fullDuration - particle.start);
	};
	for (auto y = 0; y != rows; ++y) {
		for (auto x = 0; x != columns; ++x) {
			const auto rect = QRect(x * size, y * size, size, size);
			p.setClipRect(rect);
			p.translate(rect.topLeft());
			const auto time = frame * descriptor.frameDuration;
			for (auto index = 0; index != count; ++index) {
				paintOne(particles[index], time);
			}
			p.translate(-rect.topLeft());
			if (++frame >= frames) {
				break;
			}
		}
	}
	return SpoilerMessCached(
		std::move(image),
		frames,
		descriptor.frameDuration,
		size);
}

SpoilerMessCached::SpoilerMessCached(
	QImage image,
	int framesCount,
	crl::time frameDuration,
	int canvasSize)
: _image(std::move(image))
, _frameDuration(frameDuration)
, _framesCount(framesCount)
, _canvasSize(canvasSize) {
	Expects(_frameDuration > 0);
	Expects(_framesCount > 0);
	Expects(_canvasSize > 0);
	Expects(_image.size() == QSize(
		std::min(_framesCount, kFramesPerRow) * _canvasSize,
		((_framesCount + kFramesPerRow - 1) / kFramesPerRow) * _canvasSize));
}

SpoilerMessCached::SpoilerMessCached(
	const SpoilerMessCached &mask,
	const QColor &color)
: SpoilerMessCached(
	style::colorizeImage(*mask.frame(0).image, color),
	mask.framesCount(),
	mask.frameDuration(),
	mask.canvasSize()) {
}

SpoilerMessFrame SpoilerMessCached::frame(int index) const {
	const auto row = index / kFramesPerRow;
	const auto column = index - row * kFramesPerRow;
	return {
		.image = &_image,
		.source = QRect(
			column * _canvasSize,
			row * _canvasSize,
			_canvasSize,
			_canvasSize),
	};
}

SpoilerMessFrame SpoilerMessCached::frame() const {
	return frame((crl::now() / _frameDuration) % _framesCount);
}

crl::time SpoilerMessCached::frameDuration() const {
	return _frameDuration;
}

int SpoilerMessCached::framesCount() const {
	return _framesCount;
}

int SpoilerMessCached::canvasSize() const {
	return _canvasSize;
}

QByteArray SpoilerMessCached::serialize() const {
	Expects(_frameDuration < std::numeric_limits<int32>::max());

	const auto skip = sizeof(Header);
	auto result = QByteArray(skip, Qt::Uninitialized);
	auto header = Header{
		.version = kVersion,
		.framesCount = _framesCount,
		.canvasSize = _canvasSize,
		.frameDuration = int32(_frameDuration),
	};
	const auto width = int(_image.width());
	const auto height = int(_image.height());
	auto grayscale = QImage(width, height, QImage::Format_Grayscale8);
	{
		auto tobytes = grayscale.bits();
		auto frombytes = _image.constBits();
		const auto toadd = grayscale.bytesPerLine() - width;
		const auto fromadd = _image.bytesPerLine() - (width * 4);
		for (auto y = 0; y != height; ++y) {
			for (auto x = 0; x != width; ++x) {
				*tobytes++ = *frombytes;
				frombytes += 4;
			}
			tobytes += toadd;
			frombytes += fromadd;
		}
	}

	auto device = QBuffer(&result);
	device.open(QIODevice::WriteOnly);
	device.seek(skip);
	grayscale.save(&device, "PNG");
	device.close();
	header.dataLength = result.size() - skip;
	header.dataHash = XXH32(result.data() + skip, header.dataLength, 0);
	memcpy(result.data(), &header, skip);
	return result;
}

std::optional<SpoilerMessCached> SpoilerMessCached::FromSerialized(
		QByteArray data,
		std::optional<Validator> validator) {
	const auto skip = sizeof(Header);
	const auto length = data.size();
	const auto bytes = reinterpret_cast<const uchar*>(data.constData());
	if (length <= skip) {
		return {};
	}
	auto header = Header();
	memcpy(&header, bytes, skip);
	if (header.version != kVersion
		|| header.canvasSize <= 0
		|| header.framesCount <= 0
		|| header.frameDuration <= 0
		|| (validator
			&& (validator->frameDuration != header.frameDuration
				|| validator->framesCount != header.framesCount
				|| validator->canvasSize != header.canvasSize))
		|| (skip + header.dataLength != length)
		|| (XXH32(bytes + skip, header.dataLength, 0) != header.dataHash)) {
		return {};
	}
	auto grayscale = QImage();
	if (!grayscale.loadFromData(bytes + skip, header.dataLength, "PNG")
		|| (grayscale.format() != QImage::Format_Grayscale8)) {
		return {};
	}
	const auto count = header.framesCount;
	const auto rows = (count + kFramesPerRow - 1) / kFramesPerRow;
	const auto columns = std::min(count, kFramesPerRow);
	const auto width = grayscale.width();
	const auto height = grayscale.height();
	if (QSize(width, height) != QSize(columns, rows) * header.canvasSize) {
		return {};
	}
	auto image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	{
		Assert(image.bytesPerLine() % 4 == 0);
		auto toints = reinterpret_cast<uint32*>(image.bits());
		auto frombytes = grayscale.constBits();
		const auto toadd = (image.bytesPerLine() / 4) - width;
		const auto fromadd = grayscale.bytesPerLine() - width;
		for (auto y = 0; y != height; ++y) {
			for (auto x = 0; x != width; ++x) {
				const auto byte = uint32(*frombytes++);
				*toints++ = (byte << 24) | (byte << 16) | (byte << 8) | byte;
			}
			toints += toadd;
			frombytes += fromadd;
		}

	}
	return SpoilerMessCached(
		std::move(image),
		count,
		header.frameDuration,
		header.canvasSize);
}

void PrepareDefaultSpoilerMess() {
	DefaultMaskSignal = new std::condition_variable();
	DefaultMaskMutex = new std::mutex();
	crl::async([] {
		const auto ratio = style::DevicePixelRatio();
		const auto size = style::ConvertScale(kDefaultCanvasSize) * ratio;
		auto cached = ReadDefaultMask(SpoilerMessCached::Validator{
			.frameDuration = kDefaultFrameDuration,
			.framesCount = kDefaultFramesCount,
			.canvasSize = size,
		});
		if (cached) {
			DefaultMask = new SpoilerMessCached(std::move(*cached));
		} else {
			DefaultMask = new SpoilerMessCached(GenerateSpoilerMess({
				.particleFadeInDuration = 200,
				.particleFadeOutDuration = 200,
				.particleSizeMin = style::ConvertScaleExact(1.5) * ratio,
				.particleSizeMax = style::ConvertScaleExact(2.) * ratio,
				.particleSpritesCount = 5,
				.particlesCount = 2000,
				.canvasSize = size,
				.framesCount = kDefaultFramesCount,
				.frameDuration = kDefaultFrameDuration,
			}));
		}
		auto lock = std::unique_lock(*DefaultMaskMutex);
		DefaultMaskSignal->notify_all();
		if (!cached) {
			WriteDefaultMask(*DefaultMask);
		}
	});
}

const SpoilerMessCached &DefaultSpoilerMask() {
	if (const auto result = DefaultMask.load()) {
		return *result;
	}
	Assert(DefaultMaskSignal != nullptr);
	Assert(DefaultMaskMutex != nullptr);
	while (true) {
		auto lock = std::unique_lock(*DefaultMaskMutex);
		if (const auto result = DefaultMask.load()) {
			return *result;
		}
		DefaultMaskSignal->wait(lock);
	}
}

const SpoilerMessCached &DefaultImageSpoiler() {
	static const auto result = [&] {
		const auto mask = Ui::DefaultSpoilerMask();
		const auto frame = mask.frame(0);
		auto image = QImage(
			frame.image->size(),
			QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(0, 0, 0, kImageSpoilerDarkenAlpha));
		auto p = QPainter(&image);
		p.drawImage(0, 0, *frame.image);
		p.end();
		return Ui::SpoilerMessCached(
			std::move(image),
			mask.framesCount(),
			mask.frameDuration(),
			mask.canvasSize());
	}();
	return result;
}

} // namespace Ui
