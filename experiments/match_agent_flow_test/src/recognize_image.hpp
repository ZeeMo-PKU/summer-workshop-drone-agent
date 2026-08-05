#pragma once
// recognize_image.hpp
//  机载侧图片识别模块：图片压缩 -> base64 -> 调阿里云百炼 qwen-vl-max。
//  HTTP 用 curl CLI（popen），自动读取 HTTPS_PROXY / HTTP_PROXY 环境变量，
//  因此配合"路线 B：本机 SSH 反向隧道 + 本机 HTTP 代理"即可零代码走代理。
//  依赖：nlohmann/json、OpenCV（机载已装）、/usr/bin/curl。

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace recognize {

// ---------------- 配置 ----------------
inline constexpr const char* kApiUrl =
    "https://ws-sxeumotzb6ouodsm.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions";
inline constexpr const char* kModelName = "qwen-vl-max";
// API key 只能由环境变量 DASHSCOPE_API_KEY 提供，禁止写入源码。
inline constexpr int kMaxImageDim = 1280;   // 压缩后图片最长边
inline constexpr int kJpegQuality = 85;
inline constexpr const char* kDefaultPrompt =
    "Identify the question shown in the image. "
    "Output ONLY the letter of the correct answer choice (A, B, or C). "
    "No explanation.";
// ----------------------------------------

// ---- Base64 编码（移植自 solve_image.cpp，已验证正确）----
inline std::string base64_encode(const std::vector<unsigned char>& data) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int i = 0;
    unsigned char arr3[3];
    unsigned char arr4[4];
    size_t len = data.size();
    size_t idx = 0;
    while (len--) {
        arr3[i++] = data[idx++];
        if (i == 3) {
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            arr4[3] = arr3[2] & 0x3f;
            for (i = 0; i < 4; ++i) ret += chars[arr4[i]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; ++j) arr3[j] = 0;
        arr4[0] = (arr3[0] & 0xfc) >> 2;
        arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
        arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; ++j) ret += chars[arr4[j]];
        while (i++ < 3) ret += '=';
    }
    return ret;
}

// ---- 图片 -> base64 data URI（OpenCV 压缩；失败回退原文件字节）----
inline std::string image_to_data_uri(const std::string& image_path) {
    std::ifstream file(image_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[recognize] file not found: " << image_path << std::endl;
        return "";
    }

    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (!img.empty()) {
        const int longest = std::max(img.cols, img.rows);
        if (longest > kMaxImageDim) {
            const double scale = static_cast<double>(kMaxImageDim) / longest;
            cv::resize(img, img, cv::Size(), scale, scale, cv::INTER_AREA);
        }
        std::vector<unsigned char> buf;
        if (cv::imencode(".jpg", img, buf,
                         {cv::IMWRITE_JPEG_QUALITY, kJpegQuality})) {
            return "data:image/jpeg;base64," + base64_encode(buf);
        }
        std::cerr << "[recognize] imencode failed; fallback to raw file"
                  << std::endl;
    }

    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    file.close();
    if (raw.empty()) {
        std::cerr << "[recognize] empty file: " << image_path << std::endl;
        return "";
    }
    return "data:image/jpeg;base64," + base64_encode(raw);
}

// ---- popen 执行命令并读取全部 stdout ----
inline std::string popen_capture(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    const int rc = ::pclose(pipe);
    if (rc != 0) {
        std::cerr << "[recognize] curl exit=" << rc << std::endl;
        return "";
    }
    return out;
}

inline bool write_private_file(const std::string& path,
                               const std::string& content) {
    const int fd = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::cerr << "[recognize] cannot create private temporary file: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = ::write(
            fd, content.data() + written, content.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::remove(path.c_str());
            return false;
        }
        written += static_cast<size_t>(count);
    }
    return ::close(fd) == 0;
}

// ---- 识别主函数：失败一律返回空字符串 ----
inline std::string recognizeImage(const std::string& image_path,
                                  const std::string& prompt = kDefaultPrompt,
                                  int timeout_ms = 60000) {
    const char* env_key = std::getenv("DASHSCOPE_API_KEY");
    const std::string api_key = (env_key && *env_key) ? env_key : "";
    if (api_key.empty()) {
        std::cerr << "[recognize] no API key (set DASHSCOPE_API_KEY)"
                  << std::endl;
        return "";
    }

    const std::string data_uri = image_to_data_uri(image_path);
    if (data_uri.empty()) return "";

    nlohmann::json payload = {
        {"model", kModelName},
        {"messages", nlohmann::json::array({{
            {"role", "user"},
            {"content", nlohmann::json::array({
                {{"type", "text"}, {"text", prompt}},
                {{"type", "image_url"},
                 {"image_url", {{"url", data_uri}}}}
            })}
        }})}
    };

    const std::string pid = std::to_string(::getpid());
    const std::string tmp_json = "/tmp/match_flow_recognize_" + pid + ".json";
    const std::string tmp_hdr = "/tmp/match_flow_recognize_" + pid + ".hdr";
    if (!write_private_file(tmp_json, payload.dump())) return "";
    const std::string headers =
        "Authorization: Bearer " + api_key +
        "\nContent-Type: application/json\n";
    if (!write_private_file(tmp_hdr, headers)) {
        ::remove(tmp_json.c_str());
        return "";
    }

    int secs = timeout_ms / 1000;
    if (secs < 1) secs = 1;
    const std::string cmd =
        "curl -s -m " + std::to_string(secs) +
        " -H @" + tmp_hdr +
        " --data @" + tmp_json +
        " \"" + kApiUrl + "\"";

    const std::string out = popen_capture(cmd);

    ::remove(tmp_json.c_str());
    ::remove(tmp_hdr.c_str());

    if (out.empty()) {
        std::cerr << "[recognize] no response (network/proxy? check "
                     "HTTPS_PROXY)"
                  << std::endl;
        return "";
    }

    try {
        const nlohmann::json res = nlohmann::json::parse(out);
        if (res.contains("error")) {
            std::cerr << "[recognize] API error: " << res["error"].dump()
                      << std::endl;
            return "";
        }
        return res["choices"][0]["message"]["content"].get<std::string>();
    } catch (const std::exception& e) {
        std::cerr << "[recognize] JSON parse failed: " << e.what()
                  << "\n  body: " << out.substr(0, 300) << std::endl;
        return "";
    }
}

}  // namespace recognize
