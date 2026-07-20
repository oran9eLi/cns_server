### 任务 4：实现按设备合并的脏状态模型

**文件：**

- 新增：`route_service/src/core/persistence/dirty_state.hpp/.cpp`
- 新增：`route_service/tests/test_dirty_state.cpp`
- 修改：`route_service/CMakeLists.txt`

**产出接口：**

```cpp
struct DesiredDeviceWrite {
  device::DeviceRecord record;
  std::uint64_t revision;
  bool write_metadata;
  bool write_status;
  bool write_telemetry;
  Urgency urgency;
};
class DirtyState {
 public:
  void Mark(DesiredDeviceWrite);
  void Mark(DesiredDeviceWrite,
            std::chrono::steady_clock::time_point marked_at);
  std::vector<DesiredDeviceWrite> TakeImmediate();
  std::vector<DesiredDeviceWrite> TakeTelemetryDue(
      std::chrono::steady_clock::time_point, std::chrono::seconds);
  void Complete(const DesiredDeviceWrite& taken_write);
  void Restore(DesiredDeviceWrite);
  std::size_t Size() const;
};
```

- [ ] **步骤 1：写失败测试**

覆盖同设备 100 帧只留最后值、多设备独立、立即任务优先、状态转换携带最后 dirty telemetry、在途 telemetry 不被重复携带、旧完成结果不清新 revision、同 revision 分字段完成互不清理、失败恢复、新 telemetry 独立批次等待不 degraded。

- [ ] **步骤 2：红灯、实现、绿灯**

```bash
cmake --build route_service/build-m2 --target test_dirty_state -j2
ctest --test-dir route_service/build-m2 -R '^dirty_state$' --output-on-failure
```

内部每个 vendor 只有一个最新项；telemetry 从非 dirty 变为 dirty 时记录本次 `Mark` 时间；`Complete` 只清除本次 `Take` 返回请求中不晚于已提交 revision 的字段；`Restore` 与之对称并与期间新值合并。

- [ ] **步骤 3：提交**

```bash
git add -- route_service/CMakeLists.txt route_service/src/core/persistence route_service/tests/test_dirty_state.cpp
git commit -m "feat: 增加最新状态合并持久化模型"
```

---
