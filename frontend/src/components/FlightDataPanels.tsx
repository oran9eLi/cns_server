import { Card, Empty, Progress, Tag } from "antd";
import {
  AlertTriangle,
  BatteryCharging,
  CheckCircle2,
  CircleDashed,
  ClipboardCheck,
  ScrollText,
  ShieldAlert,
  Zap
} from "lucide-react";

import { formatDateTime } from "../utils/telemetry.js";
import type {
  FlightAlertItem,
  FlightDashboardView,
  FlightLogItem,
  FlightModuleState,
  FlightPowerChannel
} from "../utils/flightDashboard.js";

export function PowerOverview({ power }: { power: FlightDashboardView["power"] }) {
  return (
    <section className="power-overview" aria-label="双电池状态">
      <PowerMetric
        label="主控电压"
        source="电池 1"
        value={formatVoltage(power.controller.voltage)}
        percent={voltagePercent(power.controller.voltage)}
        icon={<Zap size={17} />}
      />
      <PowerMetric
        label="主控电量"
        source="电池 1"
        value={formatPercent(power.controller.remaining)}
        percent={power.controller.remaining}
        icon={<BatteryCharging size={17} />}
        tone="amber"
      />
      <PowerMetric
        label="电机电压"
        source="电池 2"
        value={formatVoltage(power.motor.voltage)}
        percent={voltagePercent(power.motor.voltage)}
        icon={<Zap size={17} />}
      />
      <PowerMetric
        label="电机电量"
        source="电池 2"
        value={formatPercent(power.motor.remaining)}
        percent={power.motor.remaining}
        icon={<BatteryCharging size={17} />}
        tone="blue"
      />
    </section>
  );
}

export function SelfCheckPanel({ modules }: { modules: FlightModuleState[] }) {
  return (
    <Card
      className="flight-panel self-check-panel"
      title={<PanelTitle icon={<ClipboardCheck size={16} />} text="系统自检" />}
    >
      <div className="self-check-list">
        {modules.map((module) => (
          <div className={`self-check-item status-${module.status}`} key={module.key}>
            <span className="module-status-dot" />
            <strong>{module.label}</strong>
            <small>{module.detail}</small>
          </div>
        ))}
      </div>
    </Card>
  );
}

export function AlertPanel({ alerts }: { alerts: FlightAlertItem[] }) {
  const warningCount = alerts.filter((item) => !isInfoLevel(item.level)).length;

  return (
    <Card
      className="flight-panel alert-panel"
      title={<PanelTitle icon={<ShieldAlert size={16} />} text="告警汇总" />}
      extra={<Tag color={warningCount > 0 ? "error" : "default"}>{warningCount} 项告警</Tag>}
    >
      {alerts.length === 0 ? (
        <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="当前遥测帧无告警" />
      ) : (
        <div className="flight-event-list">
          {alerts.map((alert) => (
            <div className={`flight-event-item level-${levelClass(alert.level)}`} key={alert.id}>
              <AlertTriangle size={15} />
              <div>
                <strong>{alert.code ?? levelLabel(alert.level)}</strong>
                <span>{alert.message}</span>
                {alert.occurredAt && <small>{formatDateTime(alert.occurredAt)}</small>}
              </div>
            </div>
          ))}
        </div>
      )}
    </Card>
  );
}

export function FlightLogPanel({ logs }: { logs: FlightLogItem[] }) {
  return (
    <Card
      className="flight-panel log-panel"
      title={<PanelTitle icon={<ScrollText size={16} />} text="消息日志" />}
      extra={<Tag>{logs.length} 条</Tag>}
    >
      {logs.length === 0 ? (
        <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="当前遥测帧无日志" />
      ) : (
        <div className="flight-event-list">
          {logs.map((log) => (
            <div className={`flight-event-item level-${levelClass(log.level)}`} key={log.id}>
              {isInfoLevel(log.level) ? <CheckCircle2 size={15} /> : <CircleDashed size={15} />}
              <div>
                <strong>{levelLabel(log.level)}</strong>
                <span>{log.message}</span>
                {log.occurredAt && <small>{formatDateTime(log.occurredAt)}</small>}
              </div>
            </div>
          ))}
        </div>
      )}
    </Card>
  );
}

function PowerMetric({
  label,
  source,
  value,
  percent,
  icon,
  tone = "cyan"
}: {
  label: string;
  source: string;
  value: string;
  percent: number | null;
  icon: React.ReactNode;
  tone?: "cyan" | "amber" | "blue";
}) {
  const normalizedPercent = percent === null ? 0 : Math.max(0, Math.min(100, percent));
  return (
    <Card className={`power-metric tone-${tone}`}>
      <div className="power-metric-head">
        <span>{icon}</span>
        <div>
          <small>{source}</small>
          <strong>{label}</strong>
        </div>
      </div>
      <b>{value}</b>
      <Progress percent={normalizedPercent} showInfo={false} size="small" />
    </Card>
  );
}

function PanelTitle({ icon, text }: { icon: React.ReactNode; text: string }) {
  return <span className="flight-panel-title">{icon}{text}</span>;
}

function formatVoltage(value: number | null): string {
  if (value === null) return "--";
  const volts = value >= 1000 ? value / 1000 : value;
  return `${volts.toFixed(2)} V`;
}

function formatPercent(value: number | null): string {
  return value === null ? "--" : `${Math.round(value)}%`;
}

function voltagePercent(value: number | null): number | null {
  if (value === null) return null;
  const volts = value >= 1000 ? value / 1000 : value;
  return Math.round(((volts - 9) / 4) * 100);
}

function levelClass(value: string): string {
  const normalized = value.toLocaleLowerCase();
  if (["error", "critical", "danger", "failed", "fault"].includes(normalized)) return "error";
  if (["warning", "warn", "degraded"].includes(normalized)) return "warning";
  if (["success", "normal", "ok", "info"].includes(normalized)) return "info";
  return "default";
}

function isInfoLevel(value: string): boolean {
  return ["info", "success", "normal", "ok"].includes(value.toLocaleLowerCase());
}

function levelLabel(value: string): string {
  const labels: Record<string, string> = {
    info: "信息",
    success: "正常",
    normal: "正常",
    ok: "正常",
    warning: "警告",
    warn: "警告",
    error: "错误",
    critical: "严重"
  };
  return labels[value.toLocaleLowerCase()] ?? value;
}
