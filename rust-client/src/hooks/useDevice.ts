import { useCallback, useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import {
  api,
  type DeviceStatus,
  type MouseEvt,
  type PortInfo,
  type StatusPayload,
} from "@/lib/api";

const MAX_EVENTS = 500;
const MAX_LOG = 300;

export interface LogLine {
  id: number;
  dir: "rx" | "tx" | "sys";
  text: string;
  ts: number;
}

export function useDevice() {
  const [ports, setPorts] = useState<PortInfo[]>([]);
  const [selectedPort, setSelectedPort] = useState<string>("");
  const [connected, setConnected] = useState(false);
  const [connectError, setConnectError] = useState<string | null>(null);

  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [events, setEvents] = useState<MouseEvt[]>([]);
  const [log, setLog] = useState<LogLine[]>([]);
  const [watching, setWatching] = useState(false);

  const logId = useRef(0);

  const pushLog = useCallback((dir: LogLine["dir"], text: string) => {
    setLog((prev) => {
      const next = [...prev, { id: logId.current++, dir, text, ts: Date.now() }];
      return next.length > MAX_LOG ? next.slice(next.length - MAX_LOG) : next;
    });
  }, []);

  const refreshPorts = useCallback(async () => {
    try {
      const list = await api.listPorts();
      setPorts(list);
      setSelectedPort((cur) => {
        if (cur && list.some((p) => p.name === cur)) return cur;
        const pico = list.find((p) => p.is_pico) ?? list[0];
        return pico?.name ?? "";
      });
    } catch (e) {
      pushLog("sys", `list ports failed: ${String(e)}`);
    }
  }, [pushLog]);

  // Wire up event listeners + initial state.
  useEffect(() => {
    let alive = true;
    const unlisteners: Array<() => void> = [];

    (async () => {
      setConnected(await api.connectionState().catch(() => false));
      await refreshPorts();

      const unLine = await listen<string>("serial://line", (e) => {
        const raw = e.payload;
        pushLog("rx", raw);
        try {
          const msg = JSON.parse(raw) as Record<string, unknown>;
          if (msg.type === "status") setStatus(msg as DeviceStatus);
          else if (msg.type === "evt" && msg.kind === "mouse") {
            setEvents((prev) => {
              const next = [...prev, msg as MouseEvt];
              return next.length > MAX_EVENTS
                ? next.slice(next.length - MAX_EVENTS)
                : next;
            });
          }
        } catch {
          /* non-JSON heartbeat line — already logged */
        }
      });

      const unStatus = await listen<StatusPayload>("serial://status", (e) => {
        const s = e.payload;
        setConnected(s.connected);
        if (s.error) {
          setConnectError(s.error);
          pushLog("sys", `disconnected: ${s.error}`);
        } else {
          pushLog("sys", s.connected ? `connected ${s.port}` : "disconnected");
        }
        if (!s.connected) {
          setStatus(null);
          setWatching(false);
        }
      });

      if (!alive) {
        unLine();
        unStatus();
        return;
      }
      unlisteners.push(unLine, unStatus);
    })();

    return () => {
      alive = false;
      unlisteners.forEach((u) => u());
    };
  }, [pushLog, refreshPorts]);

  const connect = useCallback(async () => {
    if (!selectedPort) return;
    setConnectError(null);
    try {
      await api.connect(selectedPort);
      setConnected(true);
      pushLog("sys", `connecting to ${selectedPort}…`);
      // Ask for a status snapshot right away.
      setTimeout(() => api.requestStatus().catch(() => {}), 200);
    } catch (e) {
      setConnectError(String(e));
      pushLog("sys", `connect failed: ${String(e)}`);
    }
  }, [selectedPort, pushLog]);

  const disconnect = useCallback(async () => {
    await api.disconnect().catch(() => {});
    setConnected(false);
    setStatus(null);
    setWatching(false);
  }, []);

  const send = useCallback(
    async (label: string, fn: () => Promise<void>) => {
      try {
        await fn();
        pushLog("tx", label);
      } catch (e) {
        pushLog("sys", `${label} failed: ${String(e)}`);
      }
    },
    [pushLog],
  );

  const toggleWatch = useCallback(
    async (on: boolean) => {
      setWatching(on);
      await send(`watch ${on ? "on" : "off"}`, () => api.setWatch(on));
    },
    [send],
  );

  return {
    ports,
    selectedPort,
    setSelectedPort,
    refreshPorts,
    connected,
    connectError,
    connect,
    disconnect,
    status,
    events,
    setEvents,
    log,
    setLog,
    watching,
    toggleWatch,
    send,
  };
}
