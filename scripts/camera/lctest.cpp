// lctest —— gaokun3 的 libcamera 最小验证客户端。
//
// 目的（对标传感器 M11 的做法）：在包 HAL 之前，先用一个几百行的独立程序
// 证明 libcamera 的 simple 流水线 + 软件 ISP 在【这台机器上】真能出图。
// 这就是 HAL 逻辑的 90%。
//
// ★ 故意【不用】libcamera 自带的 `cam` 工具：它依赖 libevent，而那是又一个
//   要交叉编到 Android 的东西。libcamera 的公开 API 自带线程，
//   用一个 condition_variable 等就够了。
//
// 所有 API 签名都是从 refs/libcamera/include/libcamera/*.h 里核过的，没凭记忆。
//
// 用法: lctest [-r RAW|YUV] [-n 帧数] [-o 输出前缀]
#include <libcamera/libcamera.h>

#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sys/mman.h>
#include <unistd.h>

using namespace libcamera;

namespace {

std::mutex mtx;
std::condition_variable cv;
std::queue<Request *> done;

void onRequestCompleted(Request *req)
{
	if (req->status() == Request::RequestCancelled)
		return;
	std::lock_guard<std::mutex> lk(mtx);
	done.push(req);
	cv.notify_one();
}

// FrameBuffer 是一组 dmabuf plane；mmap 出来才能存盘。
bool writePlanes(const FrameBuffer *fb, const std::string &path)
{
	std::ofstream out(path, std::ios::binary);
	if (!out) {
		std::cerr << "  ✗ 打不开 " << path << "\n";
		return false;
	}
	for (const FrameBuffer::Plane &p : fb->planes()) {
		void *m = mmap(nullptr, p.offset + p.length, PROT_READ,
			       MAP_SHARED, p.fd.get(), 0);
		if (m == MAP_FAILED) {
			std::cerr << "  ✗ mmap 失败: " << strerror(errno) << "\n";
			return false;
		}
		out.write(static_cast<char *>(m) + p.offset, p.length);
		munmap(m, p.offset + p.length);
	}
	return true;
}

} /* namespace */

int main(int argc, char **argv)
{
	StreamRole role = StreamRole::Viewfinder;
	unsigned int want = 3;
	std::string prefix = "/data/local/tmp/lcframe";   /* 实际文件名是 <prefix>-last.<格式> */

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-r" && i + 1 < argc) {
			std::string v = argv[++i];
			role = (v == "RAW") ? StreamRole::Raw : StreamRole::Viewfinder;
		} else if (a == "-n" && i + 1 < argc) {
			want = std::stoul(argv[++i]);
		} else if (a == "-o" && i + 1 < argc) {
			prefix = argv[++i];
		}
	}

	std::cout << "libcamera 版本: " << CameraManager::version() << "\n";

	CameraManager cm;
	int ret = cm.start();
	if (ret) {
		std::cerr << "✗ CameraManager::start 失败: " << ret << "\n";
		return 1;
	}

	auto cams = cm.cameras();
	std::cout << "发现相机数: " << cams.size() << "\n";
	for (auto &c : cams)
		std::cout << "  - " << c->id() << "\n";
	if (cams.empty()) {
		std::cerr << "✗ 一个相机都没有 —— 检查 camss 是否 probe、"
			     "以及 LIBCAMERA_LOG_LEVELS=*:DEBUG 的输出\n";
		cm.stop();
		return 2;
	}

	std::shared_ptr<Camera> cam = cams[0];
	if (cam->acquire()) {
		std::cerr << "✗ acquire 失败\n";
		cm.stop();
		return 3;
	}

	auto cfg = cam->generateConfiguration({ role });
	if (!cfg || cfg->empty()) {
		std::cerr << "✗ generateConfiguration 给不出配置\n";
		cam->release(); cm.stop();
		return 4;
	}

	StreamConfiguration &sc = cfg->at(0);
	std::cout << "默认配置: " << sc.toString() << "\n";

	CameraConfiguration::Status st = cfg->validate();
	if (st == CameraConfiguration::Invalid) {
		std::cerr << "✗ 配置无效\n";
		cam->release(); cm.stop();
		return 5;
	}
	if (st == CameraConfiguration::Adjusted)
		std::cout << "（配置被驱动调整为）: " << sc.toString() << "\n";

	if (cam->configure(cfg.get())) {
		std::cerr << "✗ configure 失败\n";
		cam->release(); cm.stop();
		return 6;
	}

	// ★ 判据就在这一行：pixelFormat 如果是 RGB/YUV 族而不是拜耳，
	//   说明软件 ISP 真的插进了流水线并在做去拜耳。
	std::cout << "★ 最终像素格式 = " << sc.pixelFormat.toString()
		  << "  尺寸 = " << sc.size.toString()
		  << "  stride = " << sc.stride << "\n";

	Stream *stream = sc.stream();
	FrameBufferAllocator alloc(cam);
	if (alloc.allocate(stream) < 0) {
		std::cerr << "✗ 分配缓冲失败\n";
		cam->release(); cm.stop();
		return 7;
	}
	const auto &bufs = alloc.buffers(stream);
	std::cout << "缓冲数: " << bufs.size() << "\n";

	std::vector<std::unique_ptr<Request>> reqs;
	for (const auto &b : bufs) {
		auto r = cam->createRequest();
		if (!r || r->addBuffer(stream, b.get())) {
			std::cerr << "✗ 建请求失败\n";
			cam->release(); cm.stop();
			return 8;
		}
		reqs.push_back(std::move(r));
	}

	cam->requestCompleted.connect(onRequestCompleted);

	if (cam->start()) {
		std::cerr << "✗ Camera::start 失败\n";
		cam->release(); cm.stop();
		return 9;
	}
	for (auto &r : reqs)
		cam->queueRequest(r.get());

	unsigned int got = 0;
	bool timedOut = false;
	while (got < want) {
		Request *r = nullptr;
		{
			std::unique_lock<std::mutex> lk(mtx);
			// 10 秒收不到一帧就认输——比无限等好，失败要看得见。
			if (!cv.wait_for(lk, std::chrono::seconds(10),
					 [] { return !done.empty(); })) {
				timedOut = true;
				break;
			}
			r = done.front();
			done.pop();
		}

		const FrameBuffer *fb = r->buffers().begin()->second;
		const FrameMetadata &md = fb->metadata();
		std::cout << "  帧 " << got << ": seq=" << md.sequence
			  << " bytesused=";
		for (const auto &p : md.planes())
			std::cout << p.bytesused << " ";
		std::cout << "\n";

		/*
		 * ⚠️★ 存【最后一帧】而不是第 0 帧。
		 * 开流后的第一帧偶尔是不完整的：标定增益时同一档重复测，
		 * 离群值【全是偏低的、没有偏高的】（如 66/99/68），
		 * 正是 frame 0 偶发坏帧的签名。见 docs/dev/camera.md。
		 * 顺带：跑 AGC 时最后一帧也是曝光收敛得最好的那一帧。
		 */
		if (got == want - 1) {
			std::string path = prefix + "-last." +
				(sc.pixelFormat.toString());
			if (writePlanes(fb, path))
				std::cout << "  ✅ 末帧(第 " << got << " 帧)已存到 "
					  << path << "\n";
		}

		got++;
		if (got < want) {
			r->reuse(Request::ReuseBuffers);
			cam->queueRequest(r);
		}
	}

	cam->stop();
	cam->requestCompleted.disconnect(onRequestCompleted);
	cam->release();
	cm.stop();

	if (timedOut) {
		std::cerr << "✗ 等帧超时（收到 " << got << "/" << want << "）\n";
		return 10;
	}
	std::cout << "✅ 收满 " << got << " 帧\n";
	return 0;
}
