import {
  Alert,
  App as AntdApp,
  Badge,
  Button,
  Card,
  Col,
  Descriptions,
  Empty,
  Form,
  Input,
  InputNumber,
  Layout,
  Progress,
  Row,
  Select,
  Space,
  Statistic,
  Table,
  Tabs,
  Tag,
  Typography,
  notification
} from "antd";
import type { TableColumnsType } from "antd";
import { QueryClient, QueryClientProvider, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import {
  Activity,
  AlertTriangle,
  BatteryCharging,
  Cpu,
  Gauge,
  LandPlot,
  PlaneTakeoff,
  Radio,
  RefreshCw,
  Send,
  ShieldAlert,
  Square,
  Wifi
} from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { BrowserRouter, Link, Navigate, Route, Routes, useNavigate, useParams } from "react-router-dom";
import type {
  CommandStatus,
  CommandUpdatedEvent,
  ConfigCommandRequest,
  DeviceCommandRequest,
  DeviceDetail,
  DeviceSummary,
  EmergencyStopCommandRequest,
  LandCommandRequest,
  SetMotorPwmCommandRequest,
  TakeoffCommandRequest
} from "@cns/backend-protocol";

import logo from "./assets/east-tech-logo.png";
import { getDevice, getDevices, getHealth, postCommand } from "./api/client.js";
import { useRealtime } from "./realtime/useRealtime.js";
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
              path="/devices/:vendorId"
              element={
                <DeviceDetailPage
                  sessionId={realtime.sessionId}
                  connected={realtime.connected}
                  mqttReady={mqttReady}
                  commandEvents={realtime.commandEvents}
                  lastEventAt={realtime.lastEventAt}
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
  const schools = Array.from(new Set(allDevices.map((device) => device.school_name)));
  const stats = {
    total: allDevices.length,
    online: allDevices.filter((device) => device.status === "online").length,
    offline: allDevices.filter((device) => device.status === "offline").length,
    degraded: allDevices.filter((device) => device.degraded).length
  };

  const columns: TableColumnsType<DeviceSummary> = [
    {
      title: "设备",
      dataIndex: "vendor_id",
      render: (_, record) => (
        <Space direction="vertical" size={0}>
          <Typography.Text strong>{record.dcdw_label ?? "未分配角色号"}</Typography.Text>
          <Typography.Text type="secondary" className="mono">{record.vendor_id}</Typography.Text>
        </Space>
      )
    },
    { title: "学校", dataIndex: "school_name" },
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
          navigate(`/devices/${record.vendor_id}`);
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
            placeholder="搜索 vendor_id、角色号、学校"
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
          rowKey="vendor_id"
          columns={columns}
          dataSource={allDevices}
          loading={devices.isLoading}
          pagination={false}
          onRow={(record) => ({ onClick: () => navigate(`/devices/${record.vendor_id}`) })}
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
  commandEvents,
  lastEventAt
}: {
  sessionId: string | null;
  connected: boolean;
  mqttReady: boolean;
  commandEvents: CommandUpdatedEvent[];
  lastEventAt: string | null;
}) {
  const { vendorId = "" } = useParams();
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const [api, contextHolder] = notification.useNotification();
  const [commandStatus, setCommandStatus] = useState<CommandStatus | null>(null);
  const deviceQuery = useQuery({
    queryKey: ["device", vendorId],
    queryFn: () => getDevice(vendorId),
    enabled: Boolean(vendorId)
  });
  const device = deviceQuery.data?.item;

  const commandMutation = useMutation({
    mutationFn: (request: DeviceCommandRequest) => postCommand(vendorId, request),
    onSuccess: () => {
      setCommandStatus("submitted");
      api.info({ message: "命令已提交", description: "正在等待路由和设备返回执行状态。" });
      queryClient.invalidateQueries({ queryKey: ["devices"] });
    },
    onError: (error) => {
      setCommandStatus("failed");
      api.error({ message: "命令提交失败", description: error.message });
    }
  });

  const deviceCommandEvents = useMemo(
    () => commandEvents.filter((event) => event.vendor_id === vendorId),
    [commandEvents, vendorId]
  );
  const latestCommandEvent = deviceCommandEvents[0];

  useEffect(() => {
    if (!latestCommandEvent) return;
    setCommandStatus(latestCommandEvent.status);
  }, [latestCommandEvent]);

  if (!device && deviceQuery.isLoading) {
    return <Card loading />;
  }
  if (!device) {
    return <Empty description="未找到设备" />;
  }

  const telemetry = device.latest_telemetry;
  const telemetryView = mapTelemetry(telemetry);
  const commandInFlight = commandStatus !== null && ![
    "succeeded",
    "failed",
    "timeout",
    "delivery_uncertain"
  ].includes(commandStatus);
  const disabled = !connected || !mqttReady || !sessionId || device.status !== "online" || commandMutation.isPending || commandInFlight;

  type DraftCommandRequest =
    | Omit<ConfigCommandRequest, "session_id" | "client_request_id">
    | Omit<SetMotorPwmCommandRequest, "session_id" | "client_request_id">
    | Omit<TakeoffCommandRequest, "session_id" | "client_request_id">
    | Omit<LandCommandRequest, "session_id" | "client_request_id">
    | Omit<EmergencyStopCommandRequest, "session_id" | "client_request_id">;

  const submit = (request: DraftCommandRequest) => {
    if (!sessionId) return;
    commandMutation.mutate({
      ...request,
      session_id: sessionId,
      client_request_id: createUuidV4()
    } as DeviceCommandRequest);
  };

  return (
    <div className="page-stack">
      {contextHolder}
      <section className="page-heading">
        <div>
          <Typography.Text className="section-eyebrow">Device Detail</Typography.Text>
          <Typography.Title>{device.dcdw_label ?? device.vendor_id}</Typography.Title>
          <Typography.Text type="secondary" className="mono">{device.vendor_id}</Typography.Text>
        </div>
        <Space>
          <DeviceStatusTag status={device.status} degraded={device.degraded} />
          <Button onClick={() => navigate("/devices")}>返回</Button>
        </Space>
      </section>

      {device.degraded && (
        <Alert type="warning" showIcon message="该设备状态来自尚未持久化的开发事件，真实链路中应等待数据库恢复确认。" />
      )}

      <div className="detail-workspace">
        <section className="detail-main">
          <FlightSnapshot device={device} telemetry={telemetryView} lastEventAt={lastEventAt} />

          <div className="telemetry-grid">
            <TelemetryCard title="姿态" icon={<Gauge />} items={[
              ["Roll", formatNumber(telemetryView.attitude.roll, "°")],
              ["Pitch", formatNumber(telemetryView.attitude.pitch, "°")],
              ["Yaw", formatNumber(telemetryView.attitude.yaw, "°")]
            ]} />
            <TelemetryCard title="环境" icon={<Activity />} items={[
              ["温度", formatNumber(telemetryView.environment.temperature, "℃")],
              ["气压", formatNumber(telemetryView.environment.pressure, " hPa")],
              ["高度", formatNumber(telemetryView.environment.altitude, " m")]
            ]} />
            <TelemetryCard title="链路" icon={<Wifi />} items={[
              ["RSSI", formatNumber(telemetryView.link.rssi, " dBm", 0)],
              ["丢包", formatNumber(telemetryView.link.packetLoss, "%")],
              ["延迟", formatNumber(telemetryView.link.latency, " ms", 0)]
            ]} />
            <MotorCard values={telemetryView.motors.pwm} />
            <TelemetryCard title="电池" icon={<BatteryCharging />} items={[
              ["电量", formatNumber(telemetryView.battery.remaining, "%", 0)],
              ["电压", formatNumber(telemetryView.battery.voltage, " mV", 0)]
            ]} />
          </div>

          <Card className="tool-surface">
            <Tabs
              items={[
                {
                  key: "identity",
                  label: "设备身份",
                  children: (
                    <Descriptions column={2} size="small">
                      <Descriptions.Item label="学校">{device.school_name}</Descriptions.Item>
                      <Descriptions.Item label="型号">{device.model_version}</Descriptions.Item>
                      <Descriptions.Item label="注册时间">{formatDateTime(device.provisioned_at)}</Descriptions.Item>
                      <Descriptions.Item label="最后活跃">{formatDateTime(device.last_seen_at)}</Descriptions.Item>
                    </Descriptions>
                  )
                },
                {
                  key: "json",
                  label: "完整遥测",
                  children: <pre className="json-view">{JSON.stringify(telemetry ?? {}, null, 2)}</pre>
                }
              ]}
            />
          </Card>
        </section>

        <aside className="detail-side">
          <CommandReadiness connected={connected} mqttReady={mqttReady} sessionId={sessionId} device={device} />

          <Card className="command-panel" title="运行时配置">
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

          <Card className="command-panel" title="飞控控制">
            <Form layout="vertical" initialValues={{ m1: 1000, m2: 1000, m3: 1000, m4: 1000 }}>
              <Row gutter={8}>
                {["m1", "m2", "m3", "m4"].map((name, index) => (
                  <Col span={12} key={name}>
                    <Form.Item label={`PWM ${index + 1}`} name={name}>
                      <InputNumber min={1000} max={2000} step={10} style={{ width: "100%" }} />
                    </Form.Item>
                  </Col>
                ))}
              </Row>
              <Form.Item shouldUpdate>
                {({ getFieldsValue }) => (
                  <Button
                    block
                    icon={<Send size={16} />}
                    disabled={disabled}
                    onClick={() => {
                      const values = getFieldsValue();
                      submit({
                        type: "control",
                        command: "set_motor_pwm",
                        parameters: { pwm_us: [values.m1, values.m2, values.m3, values.m4] }
                      });
                    }}
                  >
                    设置四路 PWM
                  </Button>
                )}
              </Form.Item>
            </Form>
            <Space.Compact block className="flight-actions">
              <Button icon={<PlaneTakeoff size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "takeoff", parameters: {} })}>
                起飞
              </Button>
              <Button icon={<LandPlot size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "land", parameters: {} })}>
                降落
              </Button>
              <Button danger icon={<Square size={16} />} disabled={disabled} onClick={() => submit({ type: "control", command: "emergency_stop", parameters: {} })}>
                急停
              </Button>
            </Space.Compact>
          </Card>

          <Card className="command-panel" title="当前命令">
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

function FlightSnapshot({
  device,
  telemetry,
  lastEventAt
}: {
  device: DeviceDetail;
  telemetry: TelemetryView;
  lastEventAt: string | null;
}) {
  const { roll, pitch, yaw } = telemetry.attitude;
  const visualRoll = roll ?? 0;
  const visualPitch = pitch ?? 0;
  const visualYaw = yaw ?? 0;
  const linkQuality = telemetry.link.rssi === null
    ? null
    : Math.max(0, Math.min(100, Math.round((telemetry.link.rssi + 95) * 2)));

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
      <div className="snapshot-copy">
        <Typography.Text className="section-eyebrow">实时状态</Typography.Text>
        <Typography.Title level={3}>飞行姿态快照</Typography.Title>
        <Typography.Text type="secondary">最新事件：{lastEventAt ? formatDateTime(lastEventAt) : "--"}</Typography.Text>
        <div className="snapshot-tags">
          <span>Roll {formatNumber(roll, "°")}</span>
          <span>Pitch {formatNumber(pitch, "°")}</span>
          <span>Yaw {formatNumber(yaw, "°")}</span>
        </div>
      </div>
      <div className="snapshot-metrics">
        <div>
          <small>链路质量</small>
          {linkQuality === null
            ? <Typography.Text type="secondary">--</Typography.Text>
            : <Progress percent={linkQuality} size="small" strokeColor="#132B88" />}
        </div>
        <div>
          <small>设备状态</small>
          <DeviceStatusTag status={device.status} degraded={device.degraded} />
        </div>
      </div>
    </Card>
  );
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

function CommandReadiness({
  connected,
  mqttReady,
  sessionId,
  device
}: {
  connected: boolean;
  mqttReady: boolean;
  sessionId: string | null;
  device: DeviceDetail;
}) {
  const items = [
    ["实时连接", connected],
    ["路由连接", mqttReady],
    ["会话令牌", Boolean(sessionId)],
    ["目标在线", device.status === "online"],
    ["非降级态", !device.degraded]
  ] as const;

  return (
    <Card className="readiness-panel">
      <Typography.Text className="section-eyebrow">控制前检查</Typography.Text>
      <Typography.Title level={4}>命令可用性</Typography.Title>
      <div className="readiness-list">
        {items.map(([label, ok]) => (
          <div key={label} className={ok ? "is-ok" : "is-warn"}>
            <span />
            <strong>{label}</strong>
            <small>{ok ? "通过" : "受限"}</small>
          </div>
        ))}
      </div>
    </Card>
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
    dispatched: "已派发",
    in_progress: "执行中",
    succeeded: "执行成功",
    failed: "执行失败",
    timeout: "执行超时",
    delivery_uncertain: "投递不确定"
  };
  return labels[status];
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

function MotorCard({ values }: { values: Array<number | null> }) {
  return (
    <Card className="telemetry-card motor-card">
      <div className="telemetry-head">
        <span><Cpu size={18} /></span>
        <strong>电机</strong>
      </div>
      <div className="motor-bars">
        {values.map((value, index) => (
          <div key={index} className="motor-row">
            <small>M{index + 1}</small>
            <div><i style={{ width: `${value === null ? 0 : Math.min(100, value / 20)}%` }} /></div>
            <span>{value ?? "--"}</span>
          </div>
        ))}
      </div>
    </Card>
  );
}
