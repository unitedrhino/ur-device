"""生成 Watcher HTTP/TCP 生命周期适配副本；依赖锚点漂移时拒绝构建，不修改上游文件。"""
import argparse
from pathlib import Path

# 原析构顺序允许 tcp_ 的接收回调晚于 HttpClient 成员锁销毁。
HTTP_OLD = """HttpClient::~HttpClient() {
    if (connected_) {
        Close();
    }
    vEventGroupDelete(event_group_handle_);
}"""
# 被动断开先清 connected_，但回调和接收任务尚未结束，不能据此跳过 join。
TCP_OLD = """void EspTcp::Disconnect() {
    // 如果已经断开，直接返回
    if (!connected_) {
        return;
    }

    // 主动断开，需要等待接收任务退出
    DoDisconnect(true);
}"""
# 主动提前释放传输对象，保证回调访问的锁、队列及事件均仍存活。
HTTP_NEW = """HttpClient::~HttpClient() {
    Close();
    // 接收线程退出前不能销毁 HTTP 的 mutex/cv/event 等回调依赖。
    tcp_.reset();
    vEventGroupDelete(event_group_handle_);
}"""
# connected_ 只表示连接状态；任务退出事件才代表回调不再访问所有者。
TCP_NEW = """void EspTcp::Disconnect() {
    if (connected_) {
        DoDisconnect(true);
    }
    // 被动断连也必须 join；超时后释放对象会让迟到回调访问已销毁内存。
    // 接收任务自身不能等自己；未建立连接时没有接收任务可等待。
    if (receive_task_handle_ && receive_task_handle_ != xTaskGetCurrentTaskHandle()) {
        xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                           pdFALSE, pdFALSE, portMAX_DELAY);
    }
}"""


def transform(source, kind):
    """根据组件类型生成适配源；仅接受唯一且未经替换的已审查锚点。"""
    old = HTTP_OLD if kind == "http" else TCP_OLD
    if source.count(old) != 1:
        raise ValueError("HTTP/TCP 依赖生命周期锚点变化，请审查上游")
    return source.replace(old, HTTP_NEW if kind == "http" else TCP_NEW)


def main():
    """只将生成结果写入构建目录，拒绝与输入同路径。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--kind", choices=("http", "tcp"), required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        raise ValueError("禁止覆盖受管理依赖")
    result = transform(args.input.read_text(), args.kind)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)


if __name__ == "__main__":
    main()
