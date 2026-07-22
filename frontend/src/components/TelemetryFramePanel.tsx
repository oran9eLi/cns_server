import { Button, Card, Empty, Input, Tag, Typography } from "antd";
import { Braces, Check, Copy, Database, Search } from "lucide-react";
import { useMemo, useState } from "react";
import type { JsonValue } from "@cns/backend-protocol";

import { formatDateTime } from "../utils/telemetry.js";
import {
  buildTelemetryFrameGroups,
  matchesTelemetryEntry,
  summarizeTelemetryFrame,
  type TelemetryFrameEntry
} from "../utils/telemetryFrame.js";

export function TelemetryFramePanel({
  source,
  receivedAt
}: {
  source: JsonValue | null | undefined;
  receivedAt: string | null;
}) {
  const [query, setQuery] = useState("");
  const [copied, setCopied] = useState(false);
  const groups = useMemo(() => buildTelemetryFrameGroups(source), [source]);
  const summary = useMemo(() => summarizeTelemetryFrame(source, groups), [source, groups]);
  const rawFrame = useMemo(() => JSON.stringify(source ?? {}, null, 2), [source]);

  const visibleGroups = useMemo(() => {
    const normalizedQuery = query.trim().toLocaleLowerCase();
    if (!normalizedQuery) return groups;

    return groups.flatMap((group) => {
      const groupMatches = `${group.title} ${group.path}`
        .toLocaleLowerCase()
        .includes(normalizedQuery);
      const entries = groupMatches
        ? group.entries
        : group.entries.filter((entry) => matchesTelemetryEntry(entry, normalizedQuery));
      return entries.length > 0 ? [{ ...group, entries }] : [];
    });
  }, [groups, query]);

  const copyFrame = async () => {
    try {
      await navigator.clipboard.writeText(rawFrame);
    } catch {
      const textArea = document.createElement("textarea");
      textArea.value = rawFrame;
      textArea.style.position = "fixed";
      textArea.style.opacity = "0";
      document.body.appendChild(textArea);
      textArea.select();
      document.execCommand("copy");
      textArea.remove();
    }
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1600);
  };

  return (
    <Card
      className="telemetry-frame-card"
      title={(
        <span className="telemetry-frame-title">
          <Database size={17} />
          完整遥测帧
          <Tag bordered={false}>{summary.fieldCount} 个字段</Tag>
        </span>
      )}
      extra={(
        <Button
          size="small"
          icon={copied ? <Check size={14} /> : <Copy size={14} />}
          disabled={!source}
          onClick={copyFrame}
        >
          {copied ? "已复制" : "复制 JSON"}
        </Button>
      )}
    >
      <div className="telemetry-frame-summary">
        <SummaryItem label="字段总数" value={String(summary.fieldCount)} />
        <SummaryItem label="有效字段" value={`${summary.populatedCount}/${summary.fieldCount}`} />
        <SummaryItem label="数据分组" value={String(summary.groupCount)} />
        <SummaryItem label="报文大小" value={formatBytes(summary.byteCount)} />
        <SummaryItem label="接收时间" value={formatDateTime(receivedAt)} />
      </div>

      <div className="telemetry-frame-toolbar">
        <Input
          allowClear
          prefix={<Search size={15} />}
          placeholder="搜索字段名、完整路径或数值，例如 battery、PWM、1100"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
        />
        <Typography.Text type="secondary">
          默认展示报文中的全部字段，包括未知字段、空值和数组元素。
        </Typography.Text>
      </div>

      {!source ? (
        <Empty description="尚未收到完整遥测帧" />
      ) : visibleGroups.length === 0 ? (
        <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="没有匹配的遥测字段" />
      ) : (
        <div className="telemetry-frame-groups">
          {visibleGroups.map((group) => (
            <section className="telemetry-frame-group" key={group.id}>
              <header>
                <div>
                  <strong>{group.title}</strong>
                  <code>{group.path}</code>
                </div>
                <span>{group.entries.length}</span>
              </header>
              <div className="telemetry-frame-fields">
                {group.entries.map((entry) => (
                  <TelemetryField entry={entry} key={entry.path} />
                ))}
              </div>
            </section>
          ))}
        </div>
      )}

      <details className="raw-telemetry-frame">
        <summary>
          <Braces size={15} />
          查看原始 JSON
        </summary>
        <pre className="json-view">{rawFrame}</pre>
      </details>
    </Card>
  );
}

function TelemetryField({ entry }: { entry: TelemetryFrameEntry }) {
  const showUnit = entry.type === "number" && entry.unit;

  return (
    <div className={`telemetry-frame-field type-${entry.type}`}>
      <div className="telemetry-field-name">
        <span>{entry.label}</span>
        <code title={entry.path}>{entry.path}</code>
      </div>
      <div className="telemetry-field-value" title={entry.displayValue}>
        <strong>{entry.displayValue}</strong>
        {showUnit && <small>{entry.unit}</small>}
        <Tag bordered={false}>{typeLabel(entry.type)}</Tag>
      </div>
    </div>
  );
}

function SummaryItem({
  label,
  value
}: {
  label: string;
  value: string;
}) {
  return (
    <div>
      <small>{label}</small>
      <strong>{value}</strong>
    </div>
  );
}

function typeLabel(type: TelemetryFrameEntry["type"]): string {
  const labels: Record<TelemetryFrameEntry["type"], string> = {
    number: "数值",
    string: "文本",
    boolean: "布尔",
    null: "空值",
    empty: "空集合"
  };
  return labels[type];
}

function formatBytes(value: number): string {
  if (value < 1024) return `${value} B`;
  return `${(value / 1024).toFixed(1)} KB`;
}
