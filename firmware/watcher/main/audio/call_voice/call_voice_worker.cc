// 文件说明：测试固件中的模型任务、固定 PCM 队列与安全取消屏障。
#include "call_voice_worker.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include "call_voice_recognizer.h"

CallVoiceWorker::~CallVoiceWorker() {
    if (task_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cancel_ = true;
        ready_.store(false);
    }
    cv_.notify_all();
    xSemaphoreTake(stopped_, portMAX_DELAY);
    vSemaphoreDelete(stopped_);
}

bool CallVoiceWorker::Initialize() {
    if (task_ != nullptr) return true;
    stopped_ = xSemaphoreCreateBinary();
    if (stopped_ == nullptr) return false;
    if (xTaskCreatePinnedToCoreWithCaps(
            [](void* context) {
                auto* worker = static_cast<CallVoiceWorker*>(context);
                const auto stopped = worker->stopped_;
                worker->Run();
                xSemaphoreGive(stopped);
                vTaskDeleteWithCaps(nullptr);
            },
            "call_voice", 16 * 1024, this, 3, &task_,
            (CONFIG_FREERTOS_NUMBER_OF_CORES > 1 ? 1 : 0),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(stopped_);
        stopped_ = nullptr;
        task_ = nullptr;
        return false;
    }
    return true;
}

bool CallVoiceWorker::Begin(srmodel_list_t* models, std::uint32_t generation, Completion completion) {
    if (task_ == nullptr || models == nullptr || !completion) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_.load() || shutdown_) return false;
    models_ = models;
    completion_ = std::move(completion);
    head_ = count_ = 0;
    cancel_ = false;
    lost_.store(false);
    generation_.store(generation);
    pending_ = true;
    busy_.store(true);
    cv_.notify_one();
    return true;
}

bool CallVoiceWorker::Push(std::uint32_t generation, const std::int16_t* samples, std::size_t count) {
    inflight_.fetch_add(1);
    const auto leave = [this] { inflight_.fetch_sub(1); cv_.notify_one(); };
    if (!ready_.load() || generation_.load() != generation) { leave(); return false; }
    if (samples == nullptr || count == 0 || count > kFrameSamples) {
        lost_.store(true);
        leave();
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        lost_.store(true);
        leave();
        return false;
    }
    if (!ready_.load() || generation_.load() != generation) { lock.unlock(); leave(); return false; }
    if (count_ == kFrameCapacity) {
        lost_.store(true);
        lock.unlock();
        leave();
        return false;
    }
    auto& frame = frames_[(head_ + count_) % kFrameCapacity];
    std::memcpy(frame.data.data(), samples, count * sizeof(*samples));
    frame.count = count;
    ++count_;
    lock.unlock();
    leave();
    return true;
}

void CallVoiceWorker::RequestStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_.load()) return;
    cancel_ = true;
    ready_.store(false);
    cv_.notify_one();
}

void CallVoiceWorker::Run() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return pending_ || shutdown_; });
        if (shutdown_) return;
        auto* models = models_;
        const auto generation = generation_.load();
        const bool cancelled_before_start = cancel_;
        pending_ = false;
        lock.unlock();
        CallVoiceRecognizer recognizer;
        // 实机 AI 让位后仅剩约 2.65 MB，加载模型会把空闲 PSRAM 压到约 240 KB。
        // 先预留，再在加载后复核；不足时立即释放并退回电容接听。
        constexpr std::size_t kLoadReserve = 3 * 1024 * 1024;
        constexpr std::size_t kMinimumFree = 512 * 1024;
        const auto before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const bool initialized = !cancelled_before_start && before >= kLoadReserve &&
                                 recognizer.Start(models);
        const auto after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const bool enough_memory = initialized && after >= kMinimumFree;
        ESP_LOGI("CallVoiceTest", "model loaded=%d usable=%d before=%u internal=%u psram=%u",
                 initialized, enough_memory, static_cast<unsigned>(before),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(after));
        lock.lock();
        if (enough_memory && !cancel_ && !shutdown_) ready_.store(true);
        bool detected = false;
        while (ready_.load() && !cancel_ && !shutdown_ && !lost_.load()) {
            // 生产者不能等待锁；丢帧通知可能发生在等待前，定时重查避免漏唤醒。
            cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return count_ != 0 || cancel_ || shutdown_ || lost_.load();
            });
            if (cancel_ || shutdown_ || lost_.load()) break;
            if (count_ == 0) continue;
            Frame frame = frames_[head_];
            head_ = (head_ + 1) % kFrameCapacity;
            --count_;
            lock.unlock();
            const bool hit = recognizer.Feed(frame.data.data(), frame.count);
            lock.lock();
            if (hit && !cancel_ && !shutdown_ && !lost_.load()) { detected = true; break; }
        }
        ready_.store(false);
        lock.unlock();
        // 已进入 Push 的生产者退出后才能判定无丢帧，防止迟到失败落在命中之后。
        while (inflight_.load() != 0) vTaskDelay(1);
        recognizer.Stop();
        lock.lock();
        const auto outcome = cancel_ || shutdown_ ? Outcome::Cancelled
                             : (detected && !lost_.load() ? Outcome::Recognized : Outcome::Unavailable);
        ESP_LOGI("CallVoiceTest", "model released outcome=%d internal=%u psram=%u",
                 static_cast<int>(outcome),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        auto completion = std::move(completion_);
        models_ = nullptr;
        head_ = count_ = 0;
        busy_.store(false);
        const bool shutting_down = shutdown_;
        lock.unlock();
        if (!shutting_down && completion) completion(generation, outcome);
    }
}
