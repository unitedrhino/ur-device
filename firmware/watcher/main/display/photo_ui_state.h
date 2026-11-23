// 拍照提示的纯状态机：与音频 Idle/Listening 分离，以本地请求代次隔离迟到进度。
#pragma once

#include <cstdint>

// PhotoUiStage 是捕获与上传任务的显示阶段；失败终态保留原因，直到取消或新请求。
enum class PhotoUiStage { Idle, Capturing, Uploading, Waiting, CaptureFailed, UploadFailed, Unavailable };

// PhotoUiState 仅保存非敏感显示状态；调用者负责线程同步和在显示任务更新 LVGL。
class PhotoUiState {
public:
    // Begin 接受递增的非零本地代次；重复或旧开始事件不能覆盖新请求/已取消的界面。
    bool Begin(uint32_t generation) {
        if (generation == 0 || (generation_ != 0 && static_cast<int32_t>(generation - generation_) <= 0))
            return false;
        generation_ = generation;
        stage_ = PhotoUiStage::Capturing;
        return true;
    }

    // Advance 只接受当前活动请求的正向进度或失败；不让迟到等待覆盖终态或取消。
    bool Advance(uint32_t generation, PhotoUiStage stage) {
        if (generation != generation_ || !Active()) return false;
        if (stage != PhotoUiStage::Uploading && stage != PhotoUiStage::Waiting &&
            stage != PhotoUiStage::CaptureFailed && stage != PhotoUiStage::UploadFailed &&
            stage != PhotoUiStage::Unavailable) return false;
        if (static_cast<int>(stage) <= static_cast<int>(stage_)) return false;
        stage_ = stage;
        return true;
    }

    // Finish 在匹配请求进入回复或结束时释放提示；旧请求不得清除新请求提示。
    bool Finish(uint32_t generation) {
        if (generation != generation_ || stage_ == PhotoUiStage::Idle) return false;
        Cancel();
        return true;
    }

    // Cancel 用于退出/断线等本地操作，保留代次墓碑，拒绝同一请求的迟到进度。
    void Cancel() { stage_ = PhotoUiStage::Idle; }

    // Active 只表示正在拍照业务阶段，不能用于开启收音或冒充正在播报。
    bool Active() const {
        return stage_ == PhotoUiStage::Capturing || stage_ == PhotoUiStage::Uploading ||
               stage_ == PhotoUiStage::Waiting;
    }

    // Text 返回非敏感提示；空字符串表示不覆盖正常音频/时钟状态。
    const char* Text() const {
        switch (stage_) {
            case PhotoUiStage::Capturing: return "正在拍照…";
            case PhotoUiStage::Uploading: return "正在上传…";
            case PhotoUiStage::Waiting: return "正在识图…";
            case PhotoUiStage::CaptureFailed: return "拍照失败，请重试";
            case PhotoUiStage::UploadFailed: return "上传失败，请重试";
            case PhotoUiStage::Unavailable: return "暂时无法拍照，请重试";
            default: return "";
        }
    }

private:
    uint32_t generation_ = 0;  // 单调本地请求序号；不携带绑定 Token 或服务器凭据。
    PhotoUiStage stage_ = PhotoUiStage::Idle;  // 唯一提示状态，不与音频状态机共享枚举。
};
