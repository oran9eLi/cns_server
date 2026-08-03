import type { DeviceCommandRequest } from "@cns/backend-protocol";

export type TrackedCommand = {
  sessionId: string;
  deviceId: string;
  commandType: DeviceCommandRequest["type"];
  command: string | null;
};

export interface CommandTracker {
  register(requestId: string, command: TrackedCommand): void;
  get(requestId: string): TrackedCommand | undefined;
  forget(requestId: string): void;
  complete(requestId: string): void;
  close(): void;
}

export function createCommandTracker(retentionMs: number): CommandTracker {
  const commands = new Map<string, TrackedCommand>();
  const cleanupTimers = new Map<string, NodeJS.Timeout>();

  return {
    register(requestId, command) {
      const timer = cleanupTimers.get(requestId);
      if (timer) clearTimeout(timer);
      cleanupTimers.delete(requestId);
      commands.set(requestId, command);
    },
    get(requestId) {
      return commands.get(requestId);
    },
    forget(requestId) {
      const timer = cleanupTimers.get(requestId);
      if (timer) clearTimeout(timer);
      cleanupTimers.delete(requestId);
      commands.delete(requestId);
    },
    complete(requestId) {
      const previous = cleanupTimers.get(requestId);
      if (previous) clearTimeout(previous);
      const timer = setTimeout(() => {
        cleanupTimers.delete(requestId);
        commands.delete(requestId);
      }, retentionMs);
      timer.unref?.();
      cleanupTimers.set(requestId, timer);
    },
    close() {
      for (const timer of cleanupTimers.values()) clearTimeout(timer);
      cleanupTimers.clear();
      commands.clear();
    }
  };
}
