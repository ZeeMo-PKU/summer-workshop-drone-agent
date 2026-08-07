// test_fisheye.cpp — 验证鱼眼/深度/点云通道能否通过 SDK 取帧
//
// 用法: ./test_fisheye
// 依次测试: getPodCapability、FisheyePointCloud、FisheyeDepth、Front、PodVisibleLight
// 成功取到点云时，把前几帧存成 /opt/iking/match_agent/test_fisheye/<通道>.pcd
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "iking_drone_sdk.h"

static void printResult(const char* name, const iking::drone::Result& r) {
    std::printf("[%s] status=%d(%s)", name, static_cast<int>(r.status),
                iking::drone::isOk(r.status) ? "Ok" : "FAIL");
    if (!r.error.empty()) std::printf(" err=%s", r.error.c_str());
    if (!r.msg.isNull() && !r.msg.empty()) {
        const std::string sn = r.msg.get("stream_name", "").asString();
        if (!sn.empty()) std::printf(" stream=%s", sn.c_str());
    }
    std::printf("\n");
    if (!r.result_json.empty())
        std::printf("    json: %s\n", r.result_json.substr(0, 500).c_str());
}

static void savePcd(const iking::drone::FrameView& f, const std::string& path) {
    std::ofstream ofs(path);
    if (!ofs) { std::printf("    [pcd] cannot open %s\n", path.c_str()); return; }
    const int n = f.width * f.height;
    const float* xyz = reinterpret_cast<const float*>(f.data);
    ofs << "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
        << "WIDTH " << f.width << "\nHEIGHT " << f.height
        << "\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS " << n << "\nDATA ascii\n";
    for (int i = 0; i < n; ++i)
        ofs << xyz[i * 3 + 0] << ' ' << xyz[i * 3 + 1] << ' ' << xyz[i * 3 + 2] << '\n';
    ofs.close();
    std::printf("    [pcd] saved %s (%d points)\n", path.c_str(), n);
}

static void testChannel(iking::drone::Client& c, iking::drone::StreamChannelType t,
                        const char* name, bool one_way, bool is_pointcloud) {
    iking::drone::FramePoolOpenOptions opts;
    opts.one_way = one_way;
    opts.wait_worker_timeout_ms = 15000;
    iking::drone::Result r = c.openFramePool(t, opts);
    printResult(name, r);
    if (!iking::drone::isOk(r.status)) return;

    for (int i = 0; i < 3; ++i) {
        iking::drone::FrameView v;
        iking::drone::StatusCode sc = c.acquireFrame(t, v, 3000);
        if (sc != iking::drone::StatusCode::Ok) {
            std::printf("    [acquire #%d] status=%d(%s)\n", i, static_cast<int>(sc),
                        "FAIL");
            break;
        }
        std::printf("    [acquire #%d] Ok w=%d h=%d type=%d size=%d seq=%u ts=%llu\n",
                    i, v.width, v.height, v.type, v.size, static_cast<unsigned>(v.sequence),
                    static_cast<unsigned long long>(v.timestamp_ns));

        const bool pc_ok = is_pointcloud && v.data && v.type == 4 &&
                           v.size == v.width * v.height * 3 * static_cast<int>(sizeof(float));
        if (pc_ok) {
            const int n = v.width * v.height;
            const float* xyz = reinterpret_cast<const float*>(v.data);
            int valid = 0;
            double sum = 0.0, minv = 1e18, maxv = 0.0;
            double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18, minz = 1e18, maxz = -1e18;
            for (int k = 0; k < n; ++k) {
                const double x = xyz[k * 3 + 0];
                const double y = xyz[k * 3 + 1];
                const double z = xyz[k * 3 + 2];
                const double rr = std::sqrt(x * x + y * y + z * z);
                if (!(rr > 0.0) || !std::isfinite(rr)) continue;
                ++valid;
                sum += rr;
                if (rr < minv) minv = rr;
                if (rr > maxv) maxv = rr;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                if (z < minz) minz = z;
                if (z > maxz) maxz = z;
            }
            std::printf("    [pc-stats] valid=%d/%d range[min=%.3f mean=%.3f max=%.3f] "
                        "x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]\n",
                        valid, n, minv, valid ? sum / valid : 0.0, maxv,
                        minx, maxx, miny, maxy, minz, maxz);
            if (i < 2) {
                std::string path = std::string("/opt/iking/match_agent/test_fisheye/") +
                                   name + "_" + std::to_string(i) + ".pcd";
                savePcd(v, path);
            }
        }
        c.releaseFrame(t, v);
    }
    c.closeFramePool(t);
}

int main() {
    std::system("mkdir -p /opt/iking/match_agent/test_fisheye");

    iking::drone::Config cfg;
    cfg.client_id = "sdk_test_fisheye";
    iking::drone::Client c(cfg);
    if (!c.connect()) { std::printf("connect failed\n"); return 1; }
    std::printf("connected\n");

    iking::drone::Result cap = c.getPodCapability(5000);
    printResult("getPodCapability", cap);
    if (!cap.msg.isNull() && !cap.msg.empty())
        std::printf("    msg: %s\n", cap.msg.toStyledString().substr(0, 2500).c_str());

    testChannel(c, iking::drone::StreamChannelType::FisheyePointCloud,
                "FisheyePointCloud", true, true);
    testChannel(c, iking::drone::StreamChannelType::FisheyeDepth,
                "FisheyeDepth", true, false);
    testChannel(c, iking::drone::StreamChannelType::Front,
                "Front", false, false);
    testChannel(c, iking::drone::StreamChannelType::PodVisibleLight,
                "PodVisibleLight", false, false);
    testChannel(c, iking::drone::StreamChannelType::PodInfrared,
                "PodInfrared", false, false);

    c.disconnect();
    std::printf("done\n");
    return 0;
}
