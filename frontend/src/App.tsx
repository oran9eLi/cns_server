import {
  Alert,
  App as AntdApp,
  Badge,
  Button,
  Card,
  Col,
  Empty,
  Form,
  Input,
  InputNumber,
  Layout,
  Row,
  Select,
  Slider,
  Space,
  Statistic,
  Table,
  Tag,
  Typography,
  notification
} from "antd";
import type { TableColumnsType } from "antd";
import { QueryClient, QueryClientProvider, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import {
  Activity,
  AlertTriangle,
  Cpu,
  Gauge,
  LandPlot,
  MapPin,
  PlaneTakeoff,
  Radio,
  RefreshCw,
  Send,
  ShieldAlert,
  Square,
  Wifi
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { BrowserRouter, Link, Navigate, Route, Routes, useNavigate, useParams } from "react-router-dom";
import type {
  CommandStatus,
  CommandUpdatedEvent,
  ConfigCommandRequest,
  DeviceCommandRequest,
  DeviceDetail,
  DeviceSummary,
  EmergencyStopCommandRequest,
  JsonValue,
  LandCommandRequest,
  SetMotorPwmCommandRequest,
  TakeoffCommandRequest
} from "@cns/backend-protocol";

import logo from "./assets/east-tech-logo.png";
import { getDevice, getDevices, getHealth, postCommand } from "./api/client.js";
import {
  FlightLogPanel,
  PowerOverview,
  SelfCheckPanel
} from "./components/FlightDataPanels.js";
import { FlightMap } from "./components/FlightMap.js";
import { TelemetryFramePanel } from "./components/TelemetryFramePanel.js";
import { useRealtime } from "./realtime/useRealtime.js";
import { usePx4Realtime } from "./realtime/usePx4Realtime.js";
import { mapFlightDashboard, type FlightDashboardView } from "./utils/flightDashboard.js";
import {
  formatDateTime,
  formatNumber,
  mapTelemetry,
  type TelemetryView
} from "./utils/telemetry.js";
import { createUuidV4 } from "./utils/uuid.js";

const queryClient = new QueryClient();

export function App() {
  return (
    <AntdApp>
      <QueryClientProvider client={queryClient}>
        <BrowserRouter>
          <ConsoleShell />
        </BrowserRouter>
      </QueryClientProvider>
    </AntdApp>
  );
}

function ConsoleShell() {
  const realtime = useRealtime(useQueryClient());
  const health = useQuery({ queryKey: ["health"], queryFn: getHealth, refetchInterval: 10000 });
  const mqttReady = health.data?.dependencies.mqtt === "ready";

  return (
    <Layout className="console-shell">
      <Layout className="console-main">
        <header className="topbar">
          <Link className="topbar-brand" to="/devices">
            <img src={logo} alt="东创大为" />
            <span>
              <strong>CNS控制台</strong>
              <small>CNS设备管理平台</small>
            </span>
          </Link>
          <Space size={12} wrap>
            <Tag color={realtime.connected ? "success" : "error"} icon={<Wifi size={14} />}>
              {realtime.connected ? "实时通道正常" : "实时通道断开"}
            </Tag>
            <Tag color={health.data?.status === "ok" ? "blue" : "warning"} icon={<Activity size={14} />}>
              服务 {health.data?.status ?? "检查中"}
            </Tag>
            <Tag color={mqttReady ? "success" : "warning"} icon={<Radio size={14} />}>
              路由 {mqttReady ? "已连接" : "不可用"}
            </Tag>
          </Space>
        </header>
        {!realtime.connected && (
          <Alert
            className="connection-alert"
            type="warning"
            showIcon
            message="实时连接不可用，命令提交已禁用。"
          />
        )}
        <Layout.Content className="content-stage">
          <Routes>
            <Route path="/" element={<Navigate to="/devices" replace />} />
            <Route path="/devices" element={<DeviceListPage />} />
            <Route
              path="/devices/:deviceId"
              element={
                <DeviceDetailPage
                  sessionId={realtime.sessionId}
                  connected={realtime.connected}
                  mqttReady={mqttReady}
                  commandEvents={realtime.commandEvents}
                />
              }
            />
          </Routes>
        </Layout.Content>
      </Layout>
    </Layout>
  );
}

function DeviceListPage() {
  const [keyword, setKeyword] = useState("");
  const [school, setSchool] = useState<string | undefined>();
  const [status, setStatus] = useState<"online" | "offline" | undefined>();
  const navigate = useNavigate();
  const devices = useQuery({
    queryKey: ["devices", keyword, school, status],
    queryFn: () => getDevices({ keyword: keyword || undefined, school_name: school, status })
  });
  const allDevices = devices.data?.items ?? [];
  const schools = Array.from(new Set(
    allDevices
      .map((device) => device.school_name)
      .filter((value): value is string => value !== null)
  ));
  const stats = {
    total: allDevices.length,
    online: allDevices.filter((device) => device.status === "online").length,
    offline: allDevices.filter((device) => device.status === "offline").length,
    degraded: allDevices.filter((device) => device.degraded).length
  };

  const columns: TableColumnsType<DeviceSummary> = [
    {
      title: "设备",
      dataIndex: "device_id",
      render: (_, record) => (
        <Space direction="vertical" size={0}>
          <Typography.Text strong>
            {record.device_type === "flight_controller"
              ? "PX4 真实飞控"
              : record.dcdw_label ?? "未分配角色号"}
          </Typography.Text>
          <Typography.Text type="secondary" className="mono">{record.device_id}</Typography.Text>
        </Space>
      )
    },
    {
      title: "类型",
      dataIndex: "device_type",
      width: 130,
      render: (value: DeviceSummary["device_type"]) => (
        <Tag color={value === "flight_controller" ? "blue" : "default"}>
          {deviceTypeLabel(value)}
        </Tag>
      )
    },
    {
      title: "学校",
      dataIndex: "school_name",
      render: (value: string | null) => value ?? "未绑定"
    },
    { title: "型号", dataIndex: "model_version", width: 120 },
    {
      title: "状态",
      dataIndex: "status",
      width: 110,
      render: (value: DeviceSummary["status"], record) => <DeviceStatusTag status={value} degraded={record.degraded} />
    },
    {
      title: "最后活跃",
      dataIndex: "last_seen_at",
      width: 180,
      render: (value) => formatDateTime(value)
    },
    {
      title: "",
      width: 110,
      render: (_, record) => (
        <Button size="small" icon={<Send size={14} />} onClick={(event) => {
          event.stopPropagation();
          navigate(`/devices/${record.device_id}`);
        }}>
          详情
        </Button>
      )
    }
  ];

  return (
    <div className="page-stack">
      <section className="page-heading">
        <div>
          <Typography.Text className="section-eyebrow">设备管理</Typography.Text>
          <Typography.Title>设备总览</Typography.Title>
        </div>
        <Button icon={<RefreshCw size={16} />} onClick={() => devices.refetch()}>
          刷新快照
        </Button>
      </section>

      <Row gutter={[16, 16]}>
        <Col xs={12} lg={6}><MetricCard title="设备总数" value={stats.total} icon={<Cpu />} /></Col>
        <Col xs={12} lg={6}><MetricCard title="在线" value={stats.online} icon={<Radio />} tone="green" /></Col>
        <Col xs={12} lg={6}><MetricCard title="离线" value={stats.offline} icon={<ShieldAlert />} tone="red" /></Col>
        <Col xs={12} lg={6}><MetricCard title="降级" value={stats.degraded} icon={<AlertTriangle />} tone="amber" /></Col>
      </Row>

      <Card className="tool-surface">
        <Space wrap className="filter-row">
          <Input.Search
            allowClear
            placeholder="搜索设备 ID、角色号、学校"
            value={keyword}
            onChange={(event: React.ChangeEvent<HTMLInputElement>) => setKeyword(event.target.value)}
            onSearch={setKeyword}
            style={{ width: 280 }}
          />
          <Select
            allowClear
            placeholder="学校"
            value={school}
            onChange={setSchool}
            options={schools.map((value) => ({ label: value, value }))}
            style={{ width: 220 }}
          />
          <Select
            allowClear
            placeholder="在线状态"
            value={status}
            onChange={setStatus}
            options={[
              { label: "在线", value: "online" },
              { label: "离线", value: "offline" }
            ]}
            style={{ width: 150 }}
          />
        </Space>
        <Table
          rowKey="device_id"
          columns={columns}
          dataSource={allDevices}
          loading={devices.isLoading}
          pagination={false}
          onRow={(record) => ({ onClick: () => navigate(`/devices/${record.device_id}`) })}
          locale={{ emptyText: <Empty description="没有匹配设备" /> }}
        />
      </Card>
    </div>
  );
}

function DeviceDetailPage({
  sessionId,
  connected,
  mqttReady,
  commandEvents
}: {
  sessionId: string | null;
  connected: boolean;
  mqttReady: boolean;
  commandEvents: CommandUpdatedEvent[];
}) {
  const { deviceId = "" } = useParams();
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const [api, contextHolder] = notification.useNotification();
  const [commandStatus, setCommandStatus] = useState<CommandStatus | null>(null);
  const [motorPwm, setMotorPwm] = useState<[number, number, number, number]>([1000, 1000, 1000, 1000]);
  const [motorEditActive, setMotorEditActive] = useState(false);
  const [motorEditSecondsLeft, setMotorEditSecondsLeft] = useState(0);
  const motorEditTimeoutRef = useRef<number | null>(null);
  const motorEditCountdownRef = useRef<number | null>(null);
  const notifiedCommandEvents = useRef(new Set<string>());
  const deviceQuery = useQuery({
    queryKey: ["device", deviceId],
    queryFn: () => getDevice(deviceId),
    enabled: Boolean(deviceId)
  });
  const device = deviceQuery.data?.item;

  const commandMutation = useMutation({
    mutationFn: (request: DeviceCommandRequest) => postCommand(deviceId, request),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ["devices"] });
    },
    onError: (error, request) => {
      setCommandStatus("failed");
      api.error({
        key: commandNotificationKey(request.client_request_id),
        message: "命令提交失败",
        description: error.message,
        duration: 4.5
      });
    }
  });

  const deviceCommandEvents = useMemo(
    () => commandEvents.filter((event) => event.device_id === deviceId),
    [commandEvents, deviceId]
  );
  const latestCommandEvent = deviceCommandEvents[0];

  useEffect(() => {
    if (!latestCommandEvent) return;
    setCommandStatus(latestCommandEvent.status);

    const notificationKey = [
      latestCommandEvent.client_request_id,
      latestCommandEvent.status,
      latestCommandEvent.updated_at
    ].join(":");
    if (notifiedCommandEvents.current.has(notificationKey)) return;
    notifiedCommandEvents.current.add(notificationKey);

    if (latestCommandEvent.status === "submitted") return;
    const commandName = commandNameLabel(latestCommandEvent.command, latestCommandEvent.command_type);
    const occurredAt = formatDateTime(latestCommandEvent.updated_at);
    const notificationOptions = {
      key: commandNotificationKey(latestCommandEvent.client_request_id),
      duration: 4.5
    };

    if (latestCommandEvent.status === "dispatched") {
      api.info({
        ...notificationOptions,
        message: "命令已派送",
        description: `树莓派已返回应答：${commandName} · ${occurredAt}`
      });
    } else if (latestCommandEvent.status === "in_progress") {
      api.info({
        ...notificationOptions,
        message: "命令执行中",
        description: `单片机已收到命令：${commandName} · ${occurredAt}`
      });
    } else if (latestCommandEvent.status === "succeeded") {
      api.success({
        ...notificationOptions,
        message: "命令执行成功",
        description: `单片机执行完成：${commandName} · ${occurredAt}`
      });
    } else if (latestCommandEvent.status === "failed") {
      const failureReason = latestCommandEvent.error?.message
        ?? latestCommandEvent.business_status
        ?? "设备未返回失败原因";
      api.error({
        ...notificationOptions,
        message: "命令执行失败",
        description: `${failureReason} · ${commandName} · ${occurredAt}`
      });
    } else if (latestCommandEvent.status === "timeout") {
      api.warning({
        ...notificationOptions,
        message: "命令执行超时",
        description: `${commandName} · ${occurredAt}`
      });
    } else if (latestCommandEvent.status === "delivery_uncertain") {
      api.warning({
        ...notificationOptions,
        message: "命令投递状态不确定",
        description: `${commandName} · ${occurredAt}`
      });
    }
  }, [api, latestCommandEvent]);

  useEffect(() => () => {
    if (motorEditTimeoutRef.current !== null) {
      window.clearTimeout(motorEditTimeoutRef.current);
    }
    if (motorEditCountdownRef.current !== null) {
      window.clearInterval(motorEditCountdownRef.current);
    }
  }, []);

  if (!device && deviceQuery.isLoading) {
    return <Card loading />;
  }
  if (!device) {
    return <Empty description="未找到设备" />;
  }

  if (device.device_type === "flight_controller") {
    return <Px4Console device={device} onBack={() => navigate("/devices")} />;
  }

  const telemetry = device.latest_telemetry;
  const remoteId = readIdentityString(telemetry, "remote_id");
  const telemetryView = mapTelemetry(telemetry);
  const dashboardView = mapFlightDashboard(telemetry);
  const liveMotorPwm = telemetryView.motors.pwm;
  const liveMotorSliderValues = liveMotorPwm.map(normalizeMotorSliderValue) as [number, number, number, number];
  const motorPwmChanged = motorEditActive && motorPwm.some((value, index) => (
    value !== liveMotorSliderValues[index]
  ));
  const commandInFlight = commandStatus !== null && ![
    "succeeded",
    "failed",
    "timeout",
    "delivery_uncertain"
  ].includes(commandStatus);
  const disabled = !connected || !mqttReady || !sessionId ||
    device.status !== "online" || commandMutation.isPending || commandInFlight;

  type DraftCommandRequest =
    | Omit<ConfigCommandRequest, "session_id" | "client_request_id">
    | Omit<SetMotorPwmCommandRequest, "session_id" | "client_request_id">
    | Omit<TakeoffCommandRequest, "session_id" | "client_request_id">
    | Omit<LandCommandRequest, "session_id" | "client_request_id">
    | Omit<EmergencyStopCommandRequest, "session_id" | "client_request_id">;

  const submit = (request: DraftCommandRequest) => {
    if (!sessionId || device.status === "offline") return;
    const clientRequestId = createUuidV4();
    const commandName = commandNameLabel(
      request.type === "control" ? request.command : null,
      request.type
    );

    setCommandStatus("submitted");
    api.info({
      key: commandNotificationKey(clientRequestId),
      message: "命令已提交",
      description: `正在提交：${commandName}`,
      duration: 4.5
    });
    commandMutation.mutate({
      ...request,
      session_id: sessionId,
      client_request_id: clientRequestId
    } as DeviceCommandRequest);
  };

  const scheduleMotorEditReset = () => {
    if (motorEditTimeoutRef.current !== null) {
      window.clearTimeout(motorEditTimeoutRef.current);
    }
    if (motorEditCountdownRef.current !== null) {
      window.clearInterval(motorEditCountdownRef.current);
    }

    const deadline = Date.now() + 5_000;
    setMotorEditSecondsLeft(5);
    motorEditCountdownRef.current = window.setInterval(() => {
      setMotorEditSecondsLeft(Math.max(1, Math.ceil((deadline - Date.now()) / 1_000)));
    }, 200);
    motorEditTimeoutRef.current = window.setTimeout(() => {
      setMotorEditActive(false);
      setMotorEditSecondsLeft(0);
      if (motorEditCountdownRef.current !== null) {
        window.clearInterval(motorEditCountdownRef.current);
        motorEditCountdownRef.current = null;
      }
      motorEditTimeoutRef.current = null;
    }, 5_000);
  };

  const changeMotorPwm = (index: number, nextValue: number) => {
    const base = motorEditActive ? motorPwm : liveMotorSliderValues;
    const nextMotorPwm = base.map((item, itemIndex) => (
      itemIndex === index ? nextValue : item
    )) as [number, number, number, number];
    const hasChanges = nextMotorPwm.some((value, itemIndex) => (
      value !== liveMotorSliderValues[itemIndex]
    ));

    setMotorPwm(nextMotorPwm);
    if (!hasChanges) {
      if (motorEditTimeoutRef.current !== null) {
        window.clearTimeout(motorEditTimeoutRef.current);
        motorEditTimeoutRef.current = null;
      }
      if (motorEditCountdownRef.current !== null) {
        window.clearInterval(motorEditCountdownRef.current);
        motorEditCountdownRef.current = null;
      }
      setMotorEditSecondsLeft(0);
      setMotorEditActive(false);
      return;
    }
    setMotorEditActive(true);
    scheduleMotorEditReset();
  };

  const applyMotorPwm = () => {
    if (!motorPwmChanged) return;
    if (motorEditTimeoutRef.current !== null) {
      window.clearTimeout(motorEditTimeoutRef.current);
      motorEditTimeoutRef.current = null;
    }
    if (motorEditCountdownRef.current !== null) {
      window.clearInterval(motorEditCountdownRef.current);
      motorEditCountdownRef.current = null;
    }
    setMotorEditSecondsLeft(0);
    setMotorEditActive(false);
    submit({
      type: "control",
      command: "set_motor_pwm",
      parameters: { pwm_us: motorPwm }
    });
  };

  return (
    <div className="page-stack flight-console-page">
      {contextHolder}
      <section className="page-heading">
        <div>
          <Typography.Text className="section-eyebrow">设备详情</Typography.Text>
          <Typography.Title>
            {device.dcdw_label ?? device.device_id}
          </Typography.Title>
          <Typography.Text type="secondary" className="mono">{device.device_id}</Typography.Text>
        </div>
        <Button onClick={() => navigate("/devices")}>返回</Button>
      </section>

      {device.degraded && (
        <Alert type="warning" showIcon message="该设备状态来自尚未持久化的开发事件，真实链路中应等待数据库恢复确认。" />
      )}

      <Card className="identity-strip">
        <div className="device-facts-grid">
          <DeviceFact label="设备类型" value={deviceTypeLabel(device.device_type)} />
          <DeviceFact label="设备 ID" value={device.device_id} />
          <DeviceFact label="Remote ID" value={remoteId ?? "未收到"} />
          <DeviceFact label="学校" value={device.school_name ?? "未绑定"} />
          <DeviceFact label="型号" value={device.model_version} />
          <DeviceFact label="注册时间" value={formatDateTime(device.provisioned_at)} />
          <DeviceFact label="最后活跃" value={formatDateTime(device.last_seen_at)} />
          <DeviceFact label="遥测接收" value={formatDateTime(device.telemetry_received_at)} />
          <DeviceFact label="当前状态" value={<DeviceStatusTag status={device.status} degraded={device.degraded} />} />
        </div>
      </Card>

      <PowerOverview power={dashboardView.power} />

      <div className="detail-workspace">
        <section className="detail-main">
          <FlightSnapshot telemetry={telemetryView} position={dashboardView.position} />

          <div className="telemetry-grid">
            <TelemetryCard title="姿态" icon={<Gauge />} items={[
              ["Roll", formatNumber(telemetryView.attitude.roll, "°")],
              ["Pitch", formatNumber(telemetryView.attitude.pitch, "°")],
              ["Yaw", formatNumber(telemetryView.attitude.yaw, "°")]
            ]} />
            <TelemetryCard title="环境" icon={<Activity />} items={[
              ["温度", formatNumber(telemetryView.environment.temperature, "℃")],
              ["湿度", formatNumber(dashboardView.environment.humidity, "%")],
              ["气压", formatNumber(telemetryView.environment.pressure, " hPa")],
              ["高度", formatNumber(dashboardView.environment.altitude ?? telemetryView.environment.altitude, " m")]
            ]} />
            <TelemetryCard title="链路" icon={<Wifi />} items={[
              ["RSSI", formatNumber(telemetryView.link.rssi, " dBm", 0)],
              ["丢包", formatNumber(telemetryView.link.packetLoss, "%")],
              ["延迟", formatNumber(telemetryView.link.latency, " ms", 0)]
            ]} />
            <TelemetryCard title="GPS" icon={<MapPin />} items={[
              ["经度", formatCoordinate(dashboardView.position.longitudeWgs84)],
              ["纬度", formatCoordinate(dashboardView.position.latitudeWgs84)],
              ["定位", gpsFixLabel(dashboardView.position.fixType, dashboardView.position.fixValid)],
              ["卫星", formatNumber(dashboardView.position.satellites, " 颗", 0)]
            ]} />
          </div>

            <div className="flight-support-grid">
              <SelfCheckPanel modules={dashboardView.modules} />
              <div className="flight-event-stack">
                <FlightLogPanel logs={dashboardView.logs} />
              </div>
            </div>

          <TelemetryFramePanel source={telemetry} receivedAt={device.telemetry_received_at} />
        </section>

        <aside className="detail-side">
          <Card className="command-panel runtime-config-panel" title="运行时配置">
            <Form layout="vertical" initialValues={{ interval: 2000, heartbeat: 5000, reconnectDelay: 1, reconnectMax: 30 }}>
              <Form.Item label="遥测上报周期（ms）" name="interval">
                <InputNumber min={100} max={60000} step={100} style={{ width: "100%" }} />
              </Form.Item>
              <Row gutter={8}>
                <Col span={12}>
                  <Form.Item label="心跳周期（ms）" name="heartbeat">
                    <InputNumber min={100} max={60000} step={100} style={{ width: "100%" }} />
                  </Form.Item>
                </Col>
                <Col span={12}>
                  <Form.Item label="MQTT 重连初值（s）" name="reconnectDelay">
                    <InputNumber min={1} max={3600} style={{ width: "100%" }} />
                  </Form.Item>
                </Col>
                <Col span={12}>
                  <Form.Item label="MQTT 重连上限（s）" name="reconnectMax">
                    <InputNumber min={1} max={3600} style={{ width: "100%" }} />
                  </Form.Item>
                </Col>
              </Row>
              <Form.Item shouldUpdate>
                {({ getFieldsValue }) => (
                  <Button
                    block
                    type="primary"
                    icon={<Send size={16} />}
                    disabled={disabled}
                    onClick={() => {
                      const values = getFieldsValue();
                      submit({
                        type: "config",
                        parameters: {
                          telemetry_publish_interval_ms: Number(values.interval),
                          heartbeat_interval_ms: Number(values.heartbeat),
                          mqtt_reconnect_delay_s: Number(values.reconnectDelay),
                          mqtt_reconnect_delay_max_s: Number(values.reconnectMax)
                        }
                      });
                    }}
                  >
                    下发配置
                  </Button>
                )}
              </Form.Item>
            </Form>
          </Card>

          <Card className="command-panel flight-control-panel" title="电机与飞行控制">
            <div className="motor-slider-list">
              {motorPwm.map((draftValue, index) => {
                const liveValue = liveMotorPwm[index];
                const value = motorEditActive ? draftValue : normalizeMotorSliderValue(liveValue);
                const displayValue = motorEditActive ? draftValue : liveValue;
                return (
                  <div
                    className={`motor-slider-control ${motorPwmChanged ? "is-editing" : "is-following"}`}
                    key={index}
                  >
                    <div className="motor-slider-label">
                      <strong>M{index + 1}</strong>
                      <span>{displayValue === null ? "等待遥测" : `${Math.round(displayValue)} μs`}</span>
                      <small>{displayValue === null ? "--" : `${Math.round(motorPwmPercent(displayValue))}%`}</small>
                    </div>
                    <Slider
                      min={1000}
                      max={2000}
                      step={10}
                      value={value}
                      tooltip={{ formatter: (current) => `${current ?? value} μs` }}
                      onChange={(nextValue) => changeMotorPwm(index, nextValue)}
                    />
                  </div>
                );
              })}
            </div>
            <div className={`motor-edit-hint${motorPwmChanged ? " is-active" : ""}`}>
              {motorPwmChanged
                ? `PWM 已修改，请在 ${Math.max(1, motorEditSecondsLeft)} 秒内点击应用`
                : "当前显示实时输出，拖动任一滑条后可修改 PWM"}
            </div>
            <Button
              block
              type="primary"
              className="apply-pwm-button"
              icon={<Send size={16} />}
              disabled={disabled || !motorPwmChanged}
              onClick={applyMotorPwm}
            >
              应用四路 PWM
            </Button>
            <div className="flight-actions">
              <Button type="primary" icon={<PlaneTakeoff size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "takeoff", parameters: {} })}>
                起飞
              </Button>
              <Button icon={<LandPlot size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "land", parameters: {} })}>
                降落
              </Button>
              <Button danger icon={<Square size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "emergency_stop", parameters: {} })}>
                急停
              </Button>
            </div>
          </Card>

          <Card className="command-panel current-command-panel" title="当前命令">
            <Badge status={commandStatus === "failed" ? "error" : commandStatus ? "processing" : "default"} />
            <Typography.Text>{commandStatus ? commandStatusLabel(commandStatus) : "暂无命令"}</Typography.Text>
            <CommandTimeline events={deviceCommandEvents} />
            {!connected && <Alert className="panel-alert" type="warning" showIcon message="实时连接断开，命令不可提交。" />}
            {device.status !== "online" && <Alert className="panel-alert" type="error" showIcon message="设备离线，命令不可提交。" />}
          </Card>
        </aside>
      </div>
    </div>
  );
}

function Px4Console({
  device,
  onBack
}: {
  device: DeviceDetail;
  onBack: () => void;
}) {
  const realtime = usePx4Realtime(device.device_id, device.latest_telemetry);
  const telemetryView = mapTelemetry(realtime.telemetry ?? undefined);
  const dashboardView = mapFlightDashboard(realtime.telemetry);
  const remoteId = readIdentityString(realtime.telemetry, "remote_id");
  const latencyTone = realtime.stats.roundTripLatencyMs === null
    ? "default"
    : realtime.stats.roundTripLatencyMs <= 80
      ? "success"
      : realtime.stats.roundTripLatencyMs <= 150 ? "warning" : "error";

  return (
    <div className="page-stack px4-console-page flight-console-page">
      <section className="page-heading">
        <div>
          <Typography.Text className="section-eyebrow">PX4 实时飞行控制台</Typography.Text>
          <Typography.Title>PX4 真实飞控</Typography.Title>
          <Typography.Text type="secondary" className="mono">{device.device_id}</Typography.Text>
        </div>
        <Button onClick={onBack}>返回设备列表</Button>
      </section>

      <Card className="px4-link-strip">
        <Space size={[8, 8]} wrap>
          <Tag color={realtime.connected ? "success" : "error"} icon={<Wifi size={14} />}>
            WebSocket {realtime.connected ? "已连接" : "重连中"}
          </Tag>
          <Tag color={realtime.usingFastPath ? "processing" : "warning"} icon={<Radio size={14} />}>
            {realtime.usingFastPath ? "PX4 高频链路" : "1 秒遥测回退"}
          </Tag>
          <Tag color={latencyTone}>
            Web↔树莓派真实 RTT {formatRealtimeMetric(realtime.stats.roundTripLatencyMs, " ms")}
          </Tag>
          <Tag>P95 {formatRealtimeMetric(realtime.stats.roundTripP95Ms, " ms")}</Tag>
          <Tag>{realtime.stats.framesPerSecond.toFixed(1)} Hz</Tag>
          <Tag>丢弃 {realtime.stats.droppedFrames} 帧</Tag>
        </Space>
      </Card>

      {!realtime.usingFastPath && (
        <Alert
          type="warning"
          showIcon
          message="PX4 高频链路尚未收到数据"
          description="页面正在使用原有数据库遥测，不影响查看；树莓派和服务器升级后会自动切换到高频实时链路。"
        />
      )}

      <Card className="identity-strip">
        <div className="device-facts-grid">
          <DeviceFact label="设备类型" value="PX4 真实飞控" />
          <DeviceFact label="设备 ID" value={device.device_id} />
          <DeviceFact label="Remote ID" value={remoteId ?? "未收到"} />
          <DeviceFact label="学校" value={device.school_name ?? "未绑定"} />
          <DeviceFact label="当前状态" value={<DeviceStatusTag status={device.status} degraded={device.degraded} />} />
          <DeviceFact label="最后慢照" value={formatDateTime(device.telemetry_received_at)} />
        </div>
      </Card>

      <section className="px4-flight-stage">
        <FlightSnapshot telemetry={telemetryView} position={dashboardView.position} />
      </section>

      <div className="telemetry-grid px4-metric-grid">
        <TelemetryCard title="飞行姿态" icon={<Gauge />} items={[
          ["Roll", formatNumber(telemetryView.attitude.roll, "°")],
          ["Pitch", formatNumber(telemetryView.attitude.pitch, "°")],
          ["Yaw", formatNumber(telemetryView.attitude.yaw, "°")]
        ]} />
        <TelemetryCard title="位置" icon={<MapPin />} items={[
          ["经度", formatCoordinate(dashboardView.position.longitudeWgs84)],
          ["纬度", formatCoordinate(dashboardView.position.latitudeWgs84)],
          ["高度", formatNumber(telemetryView.environment.altitude, " m")],
          ["卫星", formatNumber(dashboardView.position.satellites, " 颗", 0)]
        ]} />
        <TelemetryCard title="飞控电源" icon={<Cpu />} items={[
          ["电压", formatNumber(telemetryView.battery.voltage, " V", 2)],
          ["余量", formatNumber(telemetryView.battery.remaining, "%", 0)],
          ["气压", formatNumber(telemetryView.environment.pressure, " hPa")],
          ["温度", formatNumber(telemetryView.environment.temperature, " ℃")]
        ]} />
        <TelemetryCard title="5G 真实链路 RTT" icon={<Wifi />} items={[
          ["当前", formatRealtimeMetric(realtime.stats.roundTripLatencyMs, " ms")],
          ["P50", formatRealtimeMetric(realtime.stats.roundTripP50Ms, " ms")],
          ["P95", formatRealtimeMetric(realtime.stats.roundTripP95Ms, " ms")],
          ["抖动", formatRealtimeMetric(realtime.stats.latencyJitterMs, " ms")]
        ]} />
      </div>

      <div className="px4-status-grid">
        <Card title="实时链路诊断">
          <div className="px4-diagnostics">
            <DeviceFact label="已接收" value={`${realtime.stats.receivedFrames} 帧`} />
            <DeviceFact label="序列缺口" value={`${realtime.stats.droppedFrames} 帧`} />
            <DeviceFact label="RTT 样本" value={`${realtime.stats.latencySamples} 个`} />
            <DeviceFact label="探测超时" value={`${realtime.stats.latencyTimeouts} 次`} />
            <DeviceFact label="最后实时帧" value={formatDateTime(realtime.stats.lastFrameAt)} />
            <Typography.Text type="secondary">
              真实 RTT 使用浏览器单调时钟计时，路径为浏览器→服务器→MQTT/5G→树莓派→服务器→浏览器，
              不依赖三端系统时间同步。它不包含 PX4 到树莓派的 USB 采集时间。
            </Typography.Text>
          </div>
        </Card>
        <div className="px4-status-notes">
          <Alert
            type="info"
            showIcon
            message="飞行控制保持安全隔离"
            description="本次先上线低延迟遥测控制台。航线和飞行命令仍未复用主控箱私有命令，避免误发给 PX4。"
          />
          <Alert
            type={realtime.stats.roundTripP95Ms !== null && realtime.stats.roundTripP95Ms > 150 ? "warning" : "success"}
            showIcon
            message="链路质量按 P95 判断"
            description="当前值反映瞬时往返，P95 更能反映 5G 波动；连续观察 30 个样本后再判断链路是否稳定。"
          />
        </div>
      </div>

      <details className="px4-raw-details">
        <summary>查看原始遥测帧</summary>
        <TelemetryFramePanel
          source={realtime.telemetry}
          receivedAt={realtime.stats.lastFrameAt ?? device.telemetry_received_at}
        />
      </details>
    </div>
  );
}

function FlightSnapshot({
  telemetry,
  position
}: {
  telemetry: TelemetryView;
  position: FlightDashboardView["position"];
}) {
  const { roll, pitch, yaw } = telemetry.attitude;
  const visualRoll = roll ?? 0;
  const visualPitch = pitch ?? 0;
  const visualYaw = yaw ?? 0;
  return (
    <Card className="flight-snapshot">
      <div className="instrument-panel" aria-label="飞行仪表">
        <div className="attitude-visual">
          <div className="attitude-roll-scale">
            {[-60, -30, 0, 30, 60].map((mark) => (
              <i key={mark} style={{ transform: `rotate(${mark}deg)` }} />
            ))}
          </div>
          <div className="attitude-ball" style={{ transform: `rotate(${visualRoll}deg)` }}>
            <div className="attitude-horizon" style={{ transform: `translateY(${visualPitch * 1.7}px)` }}>
              <span className="pitch-line is-top">10</span>
              <span className="pitch-line is-mid" />
              <span className="pitch-line is-bottom">10</span>
            </div>
          </div>
          <div className="attitude-aircraft">
            <span />
            <b />
            <span />
          </div>
          <div className="attitude-pointer" />
          <span className="attitude-yaw">{yaw === null ? "--" : `${Math.round(yaw)}°`}</span>
        </div>
        <CompassIndicator yaw={visualYaw} />
      </div>
      <FlightMap position={position} />
    </Card>
  );
}

function formatRealtimeMetric(value: number | null, suffix: string): string {
  return value === null ? "--" : `${value}${suffix}`;
}

function DeviceFact({ label, value }: { label: string; value: React.ReactNode }) {
  return (
    <div className="device-fact">
      <small>{label}</small>
      <strong>{value}</strong>
    </div>
  );
}

function deviceTypeLabel(type: DeviceSummary["device_type"]): string {
  return type === "flight_controller" ? "PX4 真实飞控" : "CNS 主控箱";
}

function readIdentityString(
  source: JsonValue | null | undefined,
  key: string
): string | null {
  if (!source || typeof source !== "object" || Array.isArray(source)) return null;
  const identity = source.identity;
  if (!identity || typeof identity !== "object" || Array.isArray(identity)) return null;
  const value = identity[key];
  return typeof value === "string" && value.length > 0 ? value : null;
}

function motorPwmPercent(value: number | null): number {
  if (value === null || !Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(100, (value - 1000) / 10));
}

function normalizeMotorSliderValue(value: number | null): number {
  if (value === null || !Number.isFinite(value)) return 1000;
  const clamped = Math.max(1000, Math.min(2000, value));
  return Math.round(clamped / 10) * 10;
}

function CompassIndicator({ yaw }: { yaw: number }) {
  const heading = ((Math.round(yaw) % 360) + 360) % 360;

  return (
    <div className="compass-visual" aria-label={`航向 ${heading} 度`}>
      <div className="compass-ticks">
        {Array.from({ length: 36 }, (_, index) => (
          <i
            key={index}
            className={index % 3 === 0 ? "is-major" : undefined}
            style={{ transform: `rotate(${index * 10}deg)` }}
          />
        ))}
      </div>
      <span className="compass-label is-n">N</span>
      <span className="compass-label is-e">E</span>
      <span className="compass-label is-s">S</span>
      <span className="compass-label is-w">W</span>
      <div className="compass-needle" style={{ transform: `rotate(${heading}deg)` }}>
        <span />
      </div>
      <strong className="compass-readout">{String(heading).padStart(3, "0")}</strong>
    </div>
  );
}

function CommandTimeline({ events }: { events: CommandUpdatedEvent[] }) {
  if (events.length === 0) {
    return <Empty className="timeline-empty" image={Empty.PRESENTED_IMAGE_SIMPLE} description="暂无命令事件" />;
  }

  return (
    <div className="command-timeline">
      {events.slice(0, 6).map((event) => (
        <div key={`${event.client_request_id}-${event.status}-${event.updated_at}`} className={`timeline-item status-${event.status}`}>
          <span />
          <div>
            <strong>{commandStatusLabel(event.status)}</strong>
            <small>{event.command ?? event.command_type} · {formatDateTime(event.updated_at)}</small>
            {event.error && <em>{event.error.message}</em>}
          </div>
        </div>
      ))}
    </div>
  );
}

function MetricCard({ title, value, icon, tone = "blue" }: { title: string; value: number; icon: React.ReactNode; tone?: string }) {
  return (
    <Card className={`metric-card tone-${tone}`}>
      <div className="metric-icon">{icon}</div>
      <Statistic title={title} value={value} />
    </Card>
  );
}

function DeviceStatusTag({ status, degraded }: { status: "online" | "offline"; degraded: boolean }) {
  if (degraded) return <Tag color="warning">降级</Tag>;
  return status === "online" ? <Tag color="success">在线</Tag> : <Tag color="error">离线</Tag>;
}

function commandStatusLabel(status: CommandStatus): string {
  const labels: Record<CommandStatus, string> = {
    submitted: "已提交",
    dispatched: "已派送",
    in_progress: "执行中",
    succeeded: "执行成功",
    failed: "执行失败",
    timeout: "执行超时",
    delivery_uncertain: "投递不确定"
  };
  return labels[status];
}

function commandNameLabel(
  command: CommandUpdatedEvent["command"],
  commandType: CommandUpdatedEvent["command_type"]
): string {
  if (commandType === "config") return "运行时配置";
  const labels: Record<NonNullable<CommandUpdatedEvent["command"]>, string> = {
    set_motor_pwm: "设置四路 PWM",
    takeoff: "起飞",
    land: "降落",
    emergency_stop: "急停"
  };
  return command ? labels[command] : "控制命令";
}

function commandNotificationKey(clientRequestId: string): string {
  return `command-${clientRequestId}`;
}

function TelemetryCard({ title, icon, items }: { title: string; icon: React.ReactNode; items: Array<[string, string]> }) {
  return (
    <Card className="telemetry-card">
      <div className="telemetry-head">
        <span>{icon}</span>
        <strong>{title}</strong>
      </div>
      <div className="telemetry-values">
        {items.map(([label, value]) => (
          <div key={label}>
            <small>{label}</small>
            <span>{value}</span>
          </div>
        ))}
      </div>
    </Card>
  );
}

function formatCoordinate(value: number | null): string {
  return value === null ? "--" : value.toFixed(6);
}

function gpsFixLabel(fixType: number | null, fixValid: boolean | null): string {
  if (fixValid === false || fixType === 0 || fixType === 1) return "无定位";
  const labels: Record<number, string> = {
    2: "2D 定位",
    3: "3D 定位",
    4: "DGPS",
    5: "RTK 浮点",
    6: "RTK 固定"
  };
  if (fixType !== null) return labels[fixType] ?? `定位 ${fixType}`;
  return fixValid === true ? "定位有效" : "等待定位";
}
