import { useCallback, useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import {
  api,
  type FlashBoard,
  type FlashEvent,
  type FlashFound,
  type FlashStepName,
} from "@/lib/api";

const MAX_LINES = 400;
const RECONNECT_TRIES = 8;

export type StepState = {
  status: "pending" | "running" | "ok" | "fail";
  detail: string;
  seconds: number | null;
};
export type BoardRun = Record<FlashStepName, StepState>;
export type FlashRuns = Record<FlashBoard, BoardRun>;
export type FlashPhase = "idle" | "detecting" | "flashing";

export interface FlashLine {
  id: number;
  board: FlashBoard | "";
  text: string;
}

const pending: StepState = { status: "pending", detail: "", seconds: null };
const freshRun = (): BoardRun => ({ build: pending, flash: pending, verify: pending });
const freshRuns = (): FlashRuns => ({ esp32: freshRun(), pico: freshRun() });

function parse(raw: string): FlashEvent | null {
  try {
    const v = JSON.parse(raw) as FlashEvent;
    return v && typeof v.event === "string" ? v : null;
  } catch {
    return null;
  }
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

interface Options {
  /** Is the serial link to the Pico open right now? */
  connected: boolean;
  selectedPort: string;
  refreshPorts: () => Promise<void>;
  /** Write a line to the app's console tab. */
  log: (text: string) => void;
}

/**
 * Drives the Firmware tab: detection, build + flash progress, and the serial
 * reconnect afterwards. Lives in App (not in the tab) so a run keeps updating
 * while you look at another tab.
 */
export function useFlasher(opts: Options) {
  const [phase, setPhase] = useState<FlashPhase>("idle");
  const [found, setFound] = useState<Partial<Record<FlashBoard, FlashFound>>>({});
  const [skipped, setSkipped] = useState<Partial<Record<FlashBoard, string>>>({});
  const [notes, setNotes] = useState<string[]>([]);
  const [runs, setRuns] = useState<FlashRuns>(freshRuns);
  const [lines, setLines] = useState<FlashLine[]>([]);
  const [result, setResult] = useState<{
    ok: boolean;
    results: Partial<Record<FlashBoard, "ok" | "fail">>;
  } | null>(null);
  const [error, setError] = useState<string | null>(null);

  const optsRef = useRef(opts);
  optsRef.current = opts;
  const lineId = useRef(0);
  const mode = useRef<"detect" | "flash">("detect");
  const sawSummary = useRef(false);
  const reconnectTo = useRef<string | null>(null);

  const pushLine = useCallback((board: FlashLine["board"], text: string) => {
    setLines((prev) => {
      const next = [...prev, { id: lineId.current++, board, text }];
      return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
    });
  }, []);

  // After a flash the Pico re-enumerates; put the serial link back if we had one.
  const reconnect = useCallback(async (previous: string) => {
    const { log, refreshPorts } = optsRef.current;
    await refreshPorts();
    for (let i = 0; i < RECONNECT_TRIES; i++) {
      try {
        const ports = await api.listPorts();
        const target = ports.find((p) => p.name === previous) ?? ports.find((p) => p.is_pico);
        if (!target) throw new Error("Pico port not back yet");
        await api.connect(target.name);
        log(`reconnected to ${target.name} after flashing`);
        setTimeout(() => api.requestStatus().catch(() => {}), 300);
        return;
      } catch {
        await sleep(1000);
      }
    }
    log(`could not reconnect to ${previous} after flashing - connect manually`);
  }, []);

  const finish = useCallback(
    (code: number) => {
      const wasFlash = mode.current === "flash";
      setPhase("idle");
      if (wasFlash && !sawSummary.current && code !== 0) {
        setError((prev) => prev ?? `The flash script exited with code ${code} before finishing.`);
      }
      if (wasFlash) {
        const previous = reconnectTo.current;
        reconnectTo.current = null;
        if (previous) void reconnect(previous);
        else void optsRef.current.refreshPorts();
        // Ports may have changed (e.g. BOOTSEL -> running); show what is plugged in now.
        mode.current = "detect";
        setPhase("detecting");
        api.flashDetect().catch(() => setPhase("idle"));
      }
    },
    [reconnect],
  );

  const handle = useCallback(
    (raw: string) => {
      const ev = parse(raw);
      if (!ev) return;
      switch (ev.event) {
        case "detect":
          setFound(Object.fromEntries(ev.found.map((f) => [f.board, f])));
          setSkipped(Object.fromEntries(ev.skipped));
          setNotes(ev.notes ?? []);
          break;
        case "step":
          setRuns((prev) => ({
            ...prev,
            [ev.board]: {
              ...prev[ev.board],
              [ev.step]: {
                status: ev.status === "start" ? "running" : ev.status,
                detail: ev.detail,
                seconds: ev.seconds,
              },
            },
          }));
          break;
        case "log":
          pushLine(ev.board, ev.line);
          break;
        case "summary":
          sawSummary.current = true;
          setResult({ ok: ev.ok, results: ev.results });
          break;
        case "error":
          setError(ev.message);
          break;
        case "stderr":
          setError((prev) => prev ?? ev.text);
          pushLine("", ev.text);
          break;
        case "exit":
          finish(ev.code);
          break;
      }
    },
    [pushLine, finish],
  );

  useEffect(() => {
    let alive = true;
    let unlisten: (() => void) | undefined;
    listen<string>("flash://event", (e) => handle(e.payload)).then((u) => {
      if (alive) unlisten = u;
      else u();
    });
    return () => {
      alive = false;
      unlisten?.();
    };
  }, [handle]);

  const detect = useCallback(async () => {
    mode.current = "detect";
    setError(null);
    setPhase("detecting");
    try {
      await api.flashDetect();
    } catch (e) {
      // Already running (e.g. the tab mounted twice): the run in progress will report.
      if (String(e).includes("already running")) return;
      setPhase("idle");
      setError(String(e));
    }
  }, []);

  const flash = useCallback(async (only?: FlashBoard[]) => {
    mode.current = "flash";
    sawSummary.current = false;
    reconnectTo.current = optsRef.current.connected ? optsRef.current.selectedPort : null;
    setRuns(freshRuns());
    setLines([]);
    setResult(null);
    setError(null);
    setPhase("flashing");
    try {
      await api.flashStart(only);
    } catch (e) {
      reconnectTo.current = null;
      setPhase("idle");
      setError(String(e));
    }
  }, []);

  const clearOutput = useCallback(() => setLines([]), []);

  return {
    phase,
    busy: phase !== "idle",
    found,
    skipped,
    notes,
    runs,
    lines,
    result,
    error,
    detect,
    flash,
    clearOutput,
  };
}

export type Flasher = ReturnType<typeof useFlasher>;
