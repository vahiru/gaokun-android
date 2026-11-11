/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— JPEG 输出的 EXIF（APP1）。
 *
 * 最小的 TIFF/EXIF 2.32 写入器：IFD0（厂商、型号、日期）、EXIF IFD（曝光、
 * ISO、闪光、模式、尺寸）、可选的 GPS IFD，以及可选的带 JPEG 缩略图的
 * IFD1。vendor 代码不能链接平台的 libexif。
 *
 * 方向：像素已经按 JPEG_ORIENTATION 转好（Session.cpp）。转过的写 0（未定义），
 * 没转的写 1：Android 的约定是 HAL 转了像素就不设方向标志，CTS 只把 0 当作
 * “像素已转”。
 *
 * 焦距、光圈与特性和结果里声明的一致（Metadata.h 的 lensParams()）。
 */
#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace gaokun3 {

struct ExifGps {
	double latitude = 0.0;     /* 度，北纬为正 */
	double longitude = 0.0;    /* 度，东经为正 */
	double altitude = 0.0;     /* 海拔，米 */
	int64_t timestamp = 0;     /* UTC 秒 */
	std::string method;        /* 定位方式，如 "GPS" */
};

struct ExifInfo {
	std::string make;
	std::string model;
	time_t captureTime = 0;    /* 系统时间 */
	int32_t subsecMs = -1;     /* captureTime 的毫秒部分，-1 = 无 */
	int32_t width = 0;         /* JPEG 的尺寸，旋转之后 */
	int32_t height = 0;
	int64_t exposureNs = 0;    /* 0 = unknown */
	int32_t iso = 0;           /* 0 = unknown */
	bool hasFlash = false;
	bool flashFired = false;
	uint8_t flashMode = 0;     /* EXIF 的闪光模式位：1 强制闪，2 强制不闪，3 自动 */
	bool manualExposure = false;
	bool manualWhiteBalance = false;
	bool rotated = false;      /* HAL 转过像素：方向写 0 */
	float focalLengthMm = 0.0f;   /* 0 = unknown */
	float fNumber = 0.0f;         /* 0 = unknown */
	std::optional<ExifGps> gps;
	std::vector<uint8_t> thumbnail;   /* JPEG，空 = 没有 */
};

/*
 * 给 jpeg_write_marker() 的 APP1 内容（"Exif\0\0" 后接 TIFF 数据）。缩略图
 * 放不进 64 KB 的段时不放，*withThumbnail 告诉调用方放没放；定位方式截到
 * kMaxGpsMethod 字节，其余部分总放得下。
 */
constexpr size_t kMaxGpsMethod = 64;
std::vector<uint8_t> buildExifApp1(const ExifInfo &info, bool *withThumbnail = nullptr);

} /* namespace gaokun3 */
