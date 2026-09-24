import { useEffect, useRef, type ComponentType } from "react";
import {
  Circle,
  CircleCheck,
  CircleX,
  Cpu,
  Hammer,
  Info,
  LoaderCircle,
  RefreshCw,
  Trash2,
  TriangleAlert,
  Usb,
} from "lucide-react";
import type { FlashBoard, FlashStepName } from "@/lib/api";
import type { Flasher, StepState } from "@/hooks/useFlasher";
import { cn } from "@/lib/utils";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardAction,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { ScrollArea } from "@/components/ui/scroll-area";

interface BoardMeta {
  id: FlashBoard;
  name: string;
  hint: string;
  steps: FlashStepName[];
  icon: ComponentType<{ className?: string }>;
}

const BOARDS: BoardMeta[] = [
  {
    id: "esp32",
    name: "ESP32-S3 host",
    hint: "Plug in the DevKitC's UART port (the CP210x one).",
    steps: ["build", "flash"],
    icon: Cpu,
  },
  {
    id: "pico",
    name: "Pico device",
    hint: "Plug in the Pico. A blank one needs BOOTSEL held while plugging in.",
    steps: ["build", "flash", "verify"],
    icon: Usb,
  },
];

const STEP_LABEL: Record<FlashStepName, string> = {
  build: "Build",
  flash: "Flash",
  verify: "Verify",
};

function StepPill({ name, s }: { name: FlashStepName; s: StepState }) {
  const Icon =
    s.status === "running"
      ? LoaderCircle
      : s.status === "ok"
        ? CircleCheck
        : s.status === "fail"
          ? CircleX
          : Circle;
  const timed = s.seconds != null && s.status !== "running" && s.status !== "pending";
  return (
    <span
      title={s.detail || undefined}
      className={cn(
        "inline-flex items-center gap-1 text-xs",
        s.status === "pending" && "text-muted-foreground",
        s.status === "running" && "text-primary",
        s.status === "ok" && "text-success",
        s.status === "fail" && "text-destructive",
      )}
    >
      <Icon className={cn("size-3.5", s.status === "running" && "animate-spin")} />
      {STEP_LABEL[name]}
      {timed ? <span className="tabular-nums opacity-70">{s.seconds}s</span> : null}
    </span>
  );
}

function BoardRow({ b, f }: { b: BoardMeta; f: Flasher }) {
  const found = f.found[b.id];
  const skipped = f.skipped[b.id];
  const run = f.runs[b.id];
  const touched = b.steps.some((s) => run[s].status !== "pending");
  const failedStep = b.steps.find((s) => run[s].status === "fail");
  const Icon = b.icon;

  return (
    <div className="space-y-2 rounded-lg border p-3">
      <div className="flex flex-wrap items-center gap-3">
        <div className="flex size-9 shrink-0 items-center justify-center rounded-lg bg-primary/15 text-primary">
          <Icon className="size-4" />
        </div>
        <div className="mr-auto min-w-0">
          <div className="flex items-center gap-2">
            <span className="text-sm font-medium">{b.name}</span>
            {found ? (
              <Badge variant="outline" className="border-transparent bg-success/15 text-success">
                {found.port}
              </Badge>
            ) : (
              <Badge variant="outline" className="text-muted-foreground">
                not detected
              </Badge>
            )}
          </div>
          <p className="truncate text-xs text-muted-foreground">
            {found
              ? `${found.state} · ${found.ident}`
              : skipped && skipped !== "not detected"
                ? skipped
                : b.hint}
          </p>
        </div>
        <Button
          size="sm"
          variant="secondary"
          className="gap-1.5"
          disabled={f.busy || !found}
          onClick={() => f.flash([b.id])}
        >
          <Hammer className="size-3.5" /> Build &amp; flash
        </Button>
      </div>

      {touched && (
        <div className="flex flex-wrap gap-x-4 gap-y-1 pl-12">
          {b.steps.map((s) => (
            <StepPill key={s} name={s} s={run[s]} />
          ))}
        </div>
      )}
      {failedStep && run[failedStep].detail && (
        <p className="pl-12 text-xs text-destructive">{run[failedStep].detail}</p>
      )}
    </div>
  );
}

export function FirmwarePanel({ f }: { f: Flasher }) {
  // Look for boards whenever the tab is opened (read-only, a few seconds).
  useEffect(() => {
    if (f.phase === "idle") void f.detect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const endRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    endRef.current?.scrollIntoView({ block: "end" });
  }, [f.lines.length]);

  const anyFound = Object.keys(f.found).length > 0;

  return (
    <div className="space-y-4">
      <Card>
        <CardHeader className="pb-3">
          <CardTitle className="text-sm">Firmware</CardTitle>
          <CardDescription>
            Builds the sketches with arduino-cli and flashes whichever boards are plugged in.
          </CardDescription>
          <CardAction className="flex gap-2">
            <Button
              variant="ghost"
              size="sm"
              className="gap-1.5"
              disabled={f.busy}
              onClick={() => void f.detect()}
            >
              <RefreshCw className={cn("size-3.5", f.phase === "detecting" && "animate-spin")} />
              Detect
            </Button>
            <Button
              size="sm"
              className="gap-1.5"
              disabled={f.busy || !anyFound}
              onClick={() => void f.flash()}
            >
              {f.phase === "flashing" ? (
                <LoaderCircle className="size-3.5 animate-spin" />
              ) : (
                <Hammer className="size-3.5" />
              )}
              Build &amp; flash all
            </Button>
          </CardAction>
        </CardHeader>
        <CardContent className="space-y-3">
          {BOARDS.map((b) => (
            <BoardRow key={b.id} b={b} f={f} />
          ))}
          {f.notes.map((n) => (
            <p key={n} className="flex items-start gap-2 text-xs text-muted-foreground">
              <Info className="mt-0.5 size-3.5 shrink-0" /> {n}
            </p>
          ))}
          <p className="flex items-start gap-2 text-xs text-muted-foreground">
            <Info className="mt-0.5 size-3.5 shrink-0" />
            Flashing closes the serial connection (an open COM port can't be flashed) and
            reconnects afterwards. Mouse passthrough pauses while each board is written.
          </p>
        </CardContent>
      </Card>

      {f.result && (
        <div
          className={cn(
            "flex items-center gap-2 rounded-lg border px-3 py-2 text-sm",
            f.result.ok
              ? "border-success/40 bg-success/10 text-success"
              : "border-destructive/40 bg-destructive/10 text-destructive",
          )}
        >
          {f.result.ok ? <CircleCheck className="size-4" /> : <CircleX className="size-4" />}
          {Object.entries(f.result.results)
            .map(([board, r]) => `${board}: ${r}`)
            .join(", ")}
        </div>
      )}
      {f.error && (
        <div className="flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive">
          <TriangleAlert className="mt-0.5 size-4 shrink-0" />
          <span className="break-words">{f.error}</span>
        </div>
      )}

      <Card>
        <CardHeader className="pb-3">
          <CardTitle className="text-sm">Output</CardTitle>
          <CardAction>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={f.clearOutput}>
              <Trash2 className="size-3.5" /> Clear
            </Button>
          </CardAction>
        </CardHeader>
        <CardContent>
          <ScrollArea className="h-56 rounded-md border bg-muted/30 p-3">
            <div className="space-y-0.5 font-mono text-xs">
              {f.lines.length === 0 ? (
                <p className="text-muted-foreground">Nothing yet.</p>
              ) : (
                f.lines.map((l) => (
                  <div key={l.id} className="flex gap-2">
                    <span className="shrink-0 select-none text-muted-foreground">
                      {l.board ? `[${l.board}]` : "•"}
                    </span>
                    <span className="break-all">{l.text}</span>
                  </div>
                ))
              )}
              <div ref={endRef} />
            </div>
          </ScrollArea>
        </CardContent>
      </Card>
    </div>
  );
}
