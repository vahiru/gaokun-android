/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— JPEG 输出的 EXIF（APP1），见 Exif.h。
 */
#include "Exif.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace gaokun3 {

namespace {

enum TiffType : uint16_t {
	kByte = 1,
	kAscii = 2,
	kShort = 3,
	kLong = 4,
	kRational = 5,
	kUndefined = 7,
};

void put16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back(x & 0xff);
	v.push_back(x >> 8);
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
	for (int i = 0; i < 4; i++)
		v.push_back((x >> (8 * i)) & 0xff);
}

/* 一个 IFD，小端；放不进条目的值紧跟在条目后面 */
class Ifd
{
public:
	void ascii(uint16_t tag, const std::string &s)
	{
		std::vector<uint8_t> v(s.begin(), s.end());
		v.push_back(0);
		/* 先算个数：被移走的实参可能先构造 */
		const uint32_t n = v.size();
		add(tag, kAscii, n, std::move(v));
	}
	void bytes(uint16_t tag, TiffType type, std::vector<uint8_t> v)
	{
		const uint32_t n = v.size();
		add(tag, type, n, std::move(v));
	}
	void u16(uint16_t tag, uint16_t x)
	{
		std::vector<uint8_t> v;
		put16(v, x);
		add(tag, kShort, 1, std::move(v));
	}
	void u32(uint16_t tag, uint32_t x)
	{
		std::vector<uint8_t> v;
		put32(v, x);
		add(tag, kLong, 1, std::move(v));
	}
	void rationals(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>> &r)
	{
		std::vector<uint8_t> v;
		for (const auto &[num, den] : r) {
			put32(v, num);
			put32(v, den);
		}
		add(tag, kRational, r.size(), std::move(v));
	}
	/* 改一个先前设的 LONG（排版之后才知道的偏移） */
	void setU32(uint16_t tag, uint32_t x)
	{
		for (Entry &e : entries_)
			if (e.tag == tag) {
				e.value.clear();
				put32(e.value, x);
			}
	}

	size_t size() const
	{
		size_t n = 2 + 12 * entries_.size() + 4;
		for (const Entry &e : entries_)
			if (e.value.size() > 4)
				n += (e.value.size() + 1) & ~size_t{ 1 };
		return n;
	}

	/* 放在 TIFF 数据的 base 偏移处，下一个 IFD 在 next（0：没有） */
	void write(std::vector<uint8_t> &out, uint32_t base, uint32_t next) const
	{
		std::vector<Entry> sorted = entries_;
		std::sort(sorted.begin(), sorted.end(),
			  [](const Entry &a, const Entry &b) { return a.tag < b.tag; });
		std::vector<uint8_t> data;
		uint32_t dataOff = base + 2 + 12 * sorted.size() + 4;

		put16(out, sorted.size());
		for (const Entry &e : sorted) {
			put16(out, e.tag);
			put16(out, e.type);
			put32(out, e.count);
			if (e.value.size() <= 4) {
				std::vector<uint8_t> v = e.value;
				v.resize(4, 0);
				out.insert(out.end(), v.begin(), v.end());
			} else {
				put32(out, dataOff + data.size());
				data.insert(data.end(), e.value.begin(), e.value.end());
				if (data.size() & 1)
					data.push_back(0);
			}
		}
		put32(out, next);
		out.insert(out.end(), data.begin(), data.end());
	}

	bool empty() const { return entries_.empty(); }

private:
	struct Entry {
		uint16_t tag;
		uint16_t type;
		uint32_t count;
		std::vector<uint8_t> value;
	};

	void add(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> value)
	{
		entries_.push_back({ tag, type, count, std::move(value) });
	}

	std::vector<Entry> entries_;
};

std::string formatDate(const struct tm &t)
{
	char buf[80];
	snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
		 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	return buf;
}

std::string formatOffset(long gmtoff)
{
	char buf[48];
	const long m = std::labs(gmtoff) / 60;
	snprintf(buf, sizeof(buf), "%c%02ld:%02ld", gmtoff < 0 ? '-' : '+', m / 60, m % 60);
	return buf;
}

/* 度换成度、分、秒（秒精确到 1/10000） */
std::vector<std::pair<uint32_t, uint32_t>> dms(double deg)
{
	deg = std::fabs(deg);
	const uint32_t d = static_cast<uint32_t>(deg);
	const double minutes = (deg - d) * 60.0;
	const uint32_t m = static_cast<uint32_t>(minutes);
	const uint32_t s = static_cast<uint32_t>(std::lround((minutes - m) * 60.0 * 10000.0));
	return { { d, 1 }, { m, 1 }, { s, 10000 } };
}

/* 曝光时间：以微秒为单位的有理数，约分 */
std::pair<uint32_t, uint32_t> exposureRational(int64_t ns)
{
	uint32_t num = static_cast<uint32_t>(std::max<int64_t>(ns / 1000, 1));
	uint32_t den = 1000000;
	const uint32_t g = std::gcd(num, den);
	return { num / g, den / g };
}

} /* namespace */

std::vector<uint8_t> buildExifApp1(const ExifInfo &info, bool *withThumbnail)
{
	struct tm local = {};
	localtime_r(&info.captureTime, &local);
	const std::string date = formatDate(local);
	const std::string offset = formatOffset(local.tm_gmtoff);

	Ifd ifd0, exif, gps, ifd1;
	ifd0.ascii(0x010f, info.make);
	ifd0.ascii(0x0110, info.model);
	ifd0.u16(0x0112, info.rotated ? 0 : 1);       /* 0：HAL 转过像素（见 Exif.h） */
	ifd0.rationals(0x011a, { { 72, 1 } });
	ifd0.rationals(0x011b, { { 72, 1 } });
	ifd0.u16(0x0128, 2);                          /* inches */
	ifd0.ascii(0x0132, date);
	ifd0.u16(0x0213, 1);                          /* YCbCr 居中 */
	ifd0.u32(0x8769, 0);                          /* EXIF IFD，下面再填 */
	if (info.gps)
		ifd0.u32(0x8825, 0);                  /* GPS IFD，下面再填 */

	if (info.exposureNs > 0)
		exif.rationals(0x829a, { exposureRational(info.exposureNs) });
	if (info.iso > 0)
		exif.u16(0x8827, static_cast<uint16_t>(std::min(info.iso, 65535)));
	exif.bytes(0x9000, kUndefined, { '0', '2', '3', '2' });
	exif.ascii(0x9003, date);
	exif.ascii(0x9004, date);
	exif.ascii(0x9010, offset);
	exif.ascii(0x9011, offset);
	exif.ascii(0x9012, offset);
	if (info.subsecMs >= 0) {
		char ss[8];
		snprintf(ss, sizeof(ss), "%03d", info.subsecMs % 1000);
		exif.ascii(0x9290, ss);
		exif.ascii(0x9291, ss);
		exif.ascii(0x9292, ss);
	}
	if (info.fNumber > 0.0f)
		exif.rationals(0x829d, { { static_cast<uint32_t>(std::lround(info.fNumber * 100)), 100 } });
	if (info.focalLengthMm > 0.0f)
		exif.rationals(0x920a, { { static_cast<uint32_t>(std::lround(info.focalLengthMm * 100)),
					  100 } });
	exif.bytes(0x9101, kUndefined, { 1, 2, 3, 0 });  /* Y Cb Cr */
	/* 闪光：位 0 闪了，位 3-4 模式，位 5 没有闪光灯 */
	exif.u16(0x9209, info.hasFlash ? (info.flashFired ? 1 : 0) | (info.flashMode & 3) << 3
				       : 0x20);
	exif.bytes(0xa000, kUndefined, { '0', '1', '0', '0' });
	exif.u16(0xa001, 1);                          /* sRGB */
	exif.u32(0xa002, info.width);
	exif.u32(0xa003, info.height);
	exif.u16(0xa402, info.manualExposure ? 1 : 0);
	exif.u16(0xa403, info.manualWhiteBalance ? 1 : 0);
	exif.u16(0xa406, 0);                          /* 标准场景 */

	if (info.gps) {
		const ExifGps &g = *info.gps;
		gps.bytes(0x0000, kByte, { 2, 3, 0, 0 });
		gps.ascii(0x0001, g.latitude < 0 ? "S" : "N");
		gps.rationals(0x0002, dms(g.latitude));
		gps.ascii(0x0003, g.longitude < 0 ? "W" : "E");
		gps.rationals(0x0004, dms(g.longitude));
		gps.bytes(0x0005, kByte, { static_cast<uint8_t>(g.altitude < 0 ? 1 : 0) });
		gps.rationals(0x0006, { { static_cast<uint32_t>(std::lround(std::fabs(g.altitude) * 100)),
					  100 } });
		const time_t ts = static_cast<time_t>(g.timestamp);
		struct tm utc = {};
		gmtime_r(&ts, &utc);
		gps.rationals(0x0007, { { static_cast<uint32_t>(utc.tm_hour), 1 },
					{ static_cast<uint32_t>(utc.tm_min), 1 },
					{ static_cast<uint32_t>(utc.tm_sec), 1 } });
		if (!g.method.empty()) {
			std::vector<uint8_t> m = { 'A', 'S', 'C', 'I', 'I', 0, 0, 0 };
			const size_t n = std::min(g.method.size(), kMaxGpsMethod);
			m.insert(m.end(), g.method.begin(), g.method.begin() + n);
			gps.bytes(0x001b, kUndefined, std::move(m));
		}
		char ds[48];
		snprintf(ds, sizeof(ds), "%04d:%02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1,
			 utc.tm_mday);
		gps.ascii(0x001d, ds);
	}

	/* APP1 的内容最多 65533 字节，含 "Exif\0\0" */
	constexpr size_t kMaxTiff = 65533 - 6;
	bool thumb = !info.thumbnail.empty();
	for (;;) {
		if (thumb) {
			ifd1.u16(0x0103, 6);                  /* JPEG */
			ifd1.rationals(0x011a, { { 72, 1 } });
			ifd1.rationals(0x011b, { { 72, 1 } });
			ifd1.u16(0x0128, 2);
			ifd1.u32(0x0201, 0);                  /* 缩略图偏移，下面再填 */
			ifd1.u32(0x0202, info.thumbnail.size());
		}

		/* 排列：文件头、IFD0、EXIF IFD、GPS IFD、IFD1、缩略图 */
		const uint32_t ifd0Off = 8;
		const uint32_t exifOff = ifd0Off + ifd0.size();
		const uint32_t gpsOff = exifOff + exif.size();
		const uint32_t ifd1Off = gpsOff + (info.gps ? gps.size() : 0);
		const uint32_t thumbOff = ifd1Off + (thumb ? ifd1.size() : 0);
		const size_t total = thumbOff + (thumb ? info.thumbnail.size() : 0);
		if (thumb && total > kMaxTiff) {
			thumb = false;
			ifd1 = Ifd();
			continue;
		}

		ifd0.setU32(0x8769, exifOff);
		ifd0.setU32(0x8825, gpsOff);
		ifd1.setU32(0x0201, thumbOff);

		std::vector<uint8_t> out = { 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2a, 0 };
		put32(out, ifd0Off);
		/* 偏移从 TIFF 头算起，即内容的第 6 字节 */
		std::vector<uint8_t> tiff;
		ifd0.write(tiff, ifd0Off, thumb ? ifd1Off : 0);
		exif.write(tiff, exifOff, 0);
		if (info.gps)
			gps.write(tiff, gpsOff, 0);
		if (thumb) {
			ifd1.write(tiff, ifd1Off, 0);
			tiff.insert(tiff.end(), info.thumbnail.begin(), info.thumbnail.end());
		}
		out.insert(out.end(), tiff.begin(), tiff.end());
		if (withThumbnail)
			*withThumbnail = thumb;
		return out;
	}
}

} /* namespace gaokun3 */
