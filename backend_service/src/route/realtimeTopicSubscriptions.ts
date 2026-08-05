export type RealtimeSubscriptionOperation = "subscribe" | "unsubscribe";

export type RealtimeTopicSubscriptionOptions = {
  topicFor(deviceId: string): string;
  subscribe(topic: string): Promise<void>;
  unsubscribe(topic: string): Promise<void>;
  operationTimeoutMs?: number;
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
  private connectionGeneration = 0;
  private reconcilePromise: Promise<void> | null = null;
  private rerun = false;

  constructor(private readonly options: RealtimeTopicSubscriptionOptions) {}

  setConnected(connected: boolean): void {
    this.connectionGeneration += 1;
    this.connected = connected;
    // 每次连接状态通知都开启新连接代，旧回调不得代表新连接的订阅结果。
    this.applied.clear();
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
      const generation = this.connectionGeneration;
      try {
        await this.withTimeout(this.options.unsubscribe(topic));
        if (!this.connected || generation !== this.connectionGeneration) {
          return true;
        }
        this.applied.delete(deviceId);
      } catch (error) {
        if (!this.connected || generation !== this.connectionGeneration) {
          return true;
        }
        this.options.onError("unsubscribe", topic, error);
        return false;
      }
      if (!this.connected) return true;
    }

    for (const deviceId of [...this.desired]) {
      if (this.applied.has(deviceId)) continue;
      const topic = this.options.topicFor(deviceId);
      const generation = this.connectionGeneration;
      try {
        await this.withTimeout(this.options.subscribe(topic));
        if (!this.connected || generation !== this.connectionGeneration) {
          return true;
        }
        this.applied.add(deviceId);
      } catch (error) {
        if (!this.connected || generation !== this.connectionGeneration) {
          return true;
        }
        this.options.onError("subscribe", topic, error);
        return false;
      }
      if (!this.connected) return true;
    }

    return true;
  }

  private async withTimeout(operation: Promise<void>): Promise<void> {
    const timeoutMs = this.options.operationTimeoutMs ?? 5000;
    await new Promise<void>((resolve, reject) => {
      let settled = false;
      const timeout = setTimeout(() => {
        if (settled) return;
        settled = true;
        reject(new Error(`MQTT 实时订阅操作 ${timeoutMs}ms 未完成`));
      }, timeoutMs);
      operation.then(
        () => {
          if (settled) return;
          settled = true;
          clearTimeout(timeout);
          resolve();
        },
        (error: unknown) => {
          if (settled) return;
          settled = true;
          clearTimeout(timeout);
          reject(error);
        }
      );
    });
  }
}
