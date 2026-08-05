export type TelemetryInterestChanged = (
  deviceId: string,
  interested: boolean
) => void;

/** 维护每个 WebSocket 会话的设备兴趣及设备级引用计数。 */
export class TelemetryInterestRegistry {
  private readonly devicesBySession = new Map<string, Set<string>>();
  private readonly sessionCountsByDevice = new Map<string, number>();

  constructor(private readonly onChanged: TelemetryInterestChanged) {}

  subscribe(sessionId: string, deviceId: string): void {
    let devices = this.devicesBySession.get(sessionId);
    if (!devices) {
      devices = new Set<string>();
      this.devicesBySession.set(sessionId, devices);
    }
    if (devices.has(deviceId)) return;

    devices.add(deviceId);
    const previous = this.sessionCountsByDevice.get(deviceId) ?? 0;
    this.sessionCountsByDevice.set(deviceId, previous + 1);
    if (previous === 0) {
      this.onChanged(deviceId, true);
    }
  }

  unsubscribe(sessionId: string, deviceId: string): void {
    const devices = this.devicesBySession.get(sessionId);
    if (!devices?.delete(deviceId)) return;
    if (devices.size === 0) {
      this.devicesBySession.delete(sessionId);
    }
    this.decrementDevice(deviceId);
  }

  removeSession(sessionId: string): void {
    const devices = this.devicesBySession.get(sessionId);
    if (!devices) return;
    this.devicesBySession.delete(sessionId);
    for (const deviceId of devices) {
      this.decrementDevice(deviceId);
    }
  }

  isInterested(sessionId: string, deviceId: string): boolean {
    return this.devicesBySession.get(sessionId)?.has(deviceId) ?? false;
  }

  private decrementDevice(deviceId: string): void {
    const previous = this.sessionCountsByDevice.get(deviceId);
    if (!previous) return;
    if (previous > 1) {
      this.sessionCountsByDevice.set(deviceId, previous - 1);
      return;
    }
    this.sessionCountsByDevice.delete(deviceId);
    this.onChanged(deviceId, false);
  }
}
