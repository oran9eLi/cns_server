export type RealtimeSubscriptionOperation = "subscribe" | "unsubscribe";

export type RealtimeTopicSubscriptionOptions = {
  topicFor(deviceId: string): string;
  subscribe(topic: string): Promise<void>;
  unsubscribe(topic: string): Promise<void>;
  onError(
    operation: RealtimeSubscriptionOperation,
    topic: string,
    error: unknown
  ): void;
};

/** 将浏览器兴趣集合串行收敛为当前 MQTT 连接上的精确订阅集合。 */
export class RealtimeTopicSubscriptions {
  private readonly desired = new Set<string>();
  private readonly applied = new Set<string>();
  private connected = false;
  private reconcilePromise: Promise<void> | null = null;
  private rerun = false;

  constructor(private readonly options: RealtimeTopicSubscriptionOptions) {}

  setConnected(connected: boolean): void {
    this.connected = connected;
    if (!connected) {
      this.applied.clear();
    }
    void this.schedule();
  }

  async setDesired(deviceId: string, interested: boolean): Promise<void> {
    if (interested) {
      this.desired.add(deviceId);
    } else {
      this.desired.delete(deviceId);
    }
    await this.schedule();
  }

  async settle(): Promise<void> {
    await this.schedule();
  }

  desiredDeviceIds(): string[] {
    return [...this.desired].sort();
  }

  appliedDeviceIds(): string[] {
    return [...this.applied].sort();
  }

  private schedule(): Promise<void> {
    if (this.reconcilePromise) {
      this.rerun = true;
      return this.reconcilePromise;
    }

    const running = this.reconcileLoop();
    this.reconcilePromise = running.finally(() => {
      this.reconcilePromise = null;
    });
    return this.reconcilePromise;
  }

  private async reconcileLoop(): Promise<void> {
    do {
      this.rerun = false;
      if (!(await this.reconcileOnce())) return;
    } while (this.rerun || this.needsReconcile());
  }

  private needsReconcile(): boolean {
    if (!this.connected) return false;
    if (this.desired.size !== this.applied.size) return true;
    return [...this.desired].some((deviceId) => !this.applied.has(deviceId));
  }

  private async reconcileOnce(): Promise<boolean> {
    if (!this.connected) return true;

    for (const deviceId of [...this.applied]) {
      if (this.desired.has(deviceId)) continue;
      const topic = this.options.topicFor(deviceId);
      try {
        await this.options.unsubscribe(topic);
        this.applied.delete(deviceId);
      } catch (error) {
        this.options.onError("unsubscribe", topic, error);
        return false;
      }
      if (!this.connected) return true;
    }

    for (const deviceId of [...this.desired]) {
      if (this.applied.has(deviceId)) continue;
      const topic = this.options.topicFor(deviceId);
      try {
        await this.options.subscribe(topic);
        if (this.connected) {
          this.applied.add(deviceId);
        }
      } catch (error) {
        this.options.onError("subscribe", topic, error);
        return false;
      }
      if (!this.connected) return true;
    }

    return true;
  }
}
