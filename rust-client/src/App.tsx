import { useMemo, useState } from "react";
import {
  Activity,
  ArrowDown,
  ArrowUp,
  Eye,
  EyeOff,
  Link2,
  Link2Off,
  MousePointer2,
  Move,
  RefreshCw,
  Send,
  Terminal,
  Trash2,
  Zap,
} from "lucide-react";
import { useDevice } from "@/hooks/useDevice";
import type { MouseButton } from "@/lib/api";
import { api } from "@/lib/api";
import { cn } from "@/lib/utils";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Separator } from "@/components/ui/separator";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

const BUTTONS: { id: MouseButton; label: string }[] = [
  { id: "left", label: "Left" },
  { id: "right", label: "Right" },
  { id: "middle", label: "Middle" },
  { id: "side1", label: "Side 1" },
  { id: "side2", label: "Side 2" },
];

function StatTile({
  label,
  value,
  tone = "default",
}: {
  label: string;
  value: string;
  tone?: "default" | "good" | "bad";
}) {
  return (
    <div className="rounded-lg border bg-card px-3 py-2">
      <div className="text-[11px] uppercase tracking-wide text-muted-foreground">
        {label}
      </div>
      <div
        className={cn(
          "font-mono text-lg font-semibold tabular-nums",
          tone === "good" && "text-success",
          tone === "bad" && "text-destructive",
        )}
      >
        {value}
      </div>
    </div>
  );
}

export default function App() {
  const d = useDevice();
  const [dx, setDx] = useState("100");
  const [dy, setDy] = useState("0");
  const [remapFrom, setRemapFrom] = useState<string>("side1");
  const [remapTo, setRemapTo] = useState("left");

  const statusTiles = useMemo(() => {
    const s = d.status;
    return [
      {
        label: "Uptime",
        value: s?.uptime_ms != null ? `${(s.uptime_ms / 1000).toFixed(0)}s` : "—",
        tone: "default" as const,
      },
      {
        label: "ESP link",
        value: s?.esp_alive == null ? "—" : s.esp_alive ? "alive" : "down",
        tone: s?.esp_alive ? ("good" as const) : ("bad" as const),
      },
      {
        label: "Frames OK",
        value: s?.frames_ok != null ? String(s.frames_ok) : "—",
        tone: "default" as const,
      },
      {
        label: "Frames bad",
        value: s?.frames_bad != null ? String(s.frames_bad) : "—",
        tone: s?.frames_bad ? ("bad" as const) : ("default" as const),
      },
    ];
  }, [d.status]);

  const move = (ndx: number, ndy: number) =>
    d.send(`move ${ndx},${ndy}`, () => api.move(ndx, ndy));

  return (
    <div className="min-h-screen bg-background text-foreground">
      {/* Header */}
      <header className="sticky top-0 z-10 border-b bg-background/80 backdrop-blur">
        <div className="mx-auto flex max-w-4xl items-center gap-3 px-5 py-3">
          <div className="flex size-9 items-center justify-center rounded-lg bg-primary/15 text-primary">
            <Zap className="size-5" />
          </div>
          <div className="mr-auto">
            <h1 className="text-sm font-semibold leading-tight">Superlight Control</h1>
            <p className="text-xs text-muted-foreground leading-tight">
              Pico CDC command channel
            </p>
          </div>

          <Badge
            variant="outline"
            className={cn(
              "gap-1.5 border-transparent",
              d.connected
                ? "bg-success/15 text-success"
                : "bg-muted text-muted-foreground",
            )}
          >
            <span
              className={cn(
                "size-1.5 rounded-full",
                d.connected ? "bg-success animate-pulse" : "bg-muted-foreground",
              )}
            />
            {d.connected ? "Connected" : "Disconnected"}
          </Badge>
        </div>
      </header>

      <main className="mx-auto max-w-4xl space-y-5 px-5 py-6">
        {/* Connection */}
        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="text-base">Connection</CardTitle>
            <CardDescription>
              Pick the Pico's serial port (CDC) and connect.
            </CardDescription>
          </CardHeader>
          <CardContent className="flex flex-wrap items-end gap-3">
            <div className="grid min-w-56 flex-1 gap-1.5">
              <Label className="text-xs">Serial port</Label>
              <Select
                value={d.selectedPort}
                onValueChange={d.setSelectedPort}
                disabled={d.connected}
              >
                <SelectTrigger>
                  <SelectValue placeholder="No ports found" />
                </SelectTrigger>
                <SelectContent>
                  {d.ports.map((p) => (
                    <SelectItem key={p.name} value={p.name}>
                      <span className="flex items-center gap-2">
                        {p.is_pico && (
                          <Badge
                            variant="secondary"
                            className="h-4 px-1 text-[10px]"
                          >
                            Pico
                          </Badge>
                        )}
                        {p.label}
                      </span>
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>

            <Button
              variant="outline"
              size="icon"
              onClick={d.refreshPorts}
              disabled={d.connected}
              title="Refresh ports"
            >
              <RefreshCw className="size-4" />
            </Button>

            {d.connected ? (
              <Button variant="destructive" onClick={d.disconnect} className="gap-2">
                <Link2Off className="size-4" />
                Disconnect
              </Button>
            ) : (
              <Button
                onClick={d.connect}
                disabled={!d.selectedPort}
                className="gap-2"
              >
                <Link2 className="size-4" />
                Connect
              </Button>
            )}
          </CardContent>
          {d.connectError && !d.connected && (
            <CardContent className="pt-0">
              <p className="text-xs text-destructive">{d.connectError}</p>
            </CardContent>
          )}
        </Card>

        {/* Status */}
        <Card>
          <CardHeader className="flex-row items-center justify-between space-y-0 pb-3">
            <CardTitle className="flex items-center gap-2 text-base">
              <Activity className="size-4 text-primary" />
              Device status
            </CardTitle>
            <Button
              variant="ghost"
              size="sm"
              className="gap-1.5"
              disabled={!d.connected}
              onClick={() => d.send("status", () => api.requestStatus())}
            >
              <RefreshCw className="size-3.5" />
              Refresh
            </Button>
          </CardHeader>
          <CardContent className="grid grid-cols-2 gap-3 sm:grid-cols-4">
            {statusTiles.map((t) => (
              <StatTile key={t.label} {...t} />
            ))}
          </CardContent>
        </Card>

        {/* Controls */}
        <Tabs defaultValue="control">
          <TabsList className="grid w-full grid-cols-4">
            <TabsTrigger value="control">Control</TabsTrigger>
            <TabsTrigger value="watch">Watch</TabsTrigger>
            <TabsTrigger value="remap">Remap</TabsTrigger>
            <TabsTrigger value="console">Console</TabsTrigger>
          </TabsList>

          {/* CONTROL */}
          <TabsContent value="control" className="mt-4">
            <fieldset
              disabled={!d.connected}
              className="grid gap-4 disabled:opacity-50 md:grid-cols-2"
            >
              {/* Move */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="flex items-center gap-2 text-sm">
                    <Move className="size-4" /> Move
                  </CardTitle>
                </CardHeader>
                <CardContent className="space-y-3">
                  <div className="grid grid-cols-2 gap-3">
                    <div className="grid gap-1.5">
                      <Label className="text-xs">dx</Label>
                      <Input
                        value={dx}
                        onChange={(e) => setDx(e.target.value)}
                        inputMode="numeric"
                      />
                    </div>
                    <div className="grid gap-1.5">
                      <Label className="text-xs">dy</Label>
                      <Input
                        value={dy}
                        onChange={(e) => setDy(e.target.value)}
                        inputMode="numeric"
                      />
                    </div>
                  </div>
                  <Button
                    className="w-full gap-2"
                    onClick={() => move(Number(dx) || 0, Number(dy) || 0)}
                  >
                    <Send className="size-4" /> Send move
                  </Button>
                  <Separator />
                  <div className="grid grid-cols-3 gap-2">
                    <div />
                    <Button variant="secondary" onClick={() => move(0, -50)}>
                      <ArrowUp className="size-4" />
                    </Button>
                    <div />
                    <Button variant="secondary" onClick={() => move(-50, 0)}>
                      ←
                    </Button>
                    <Button variant="secondary" onClick={() => move(0, 50)}>
                      <ArrowDown className="size-4" />
                    </Button>
                    <Button variant="secondary" onClick={() => move(50, 0)}>
                      →
                    </Button>
                  </div>
                </CardContent>
              </Card>

              {/* Click + scroll */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="flex items-center gap-2 text-sm">
                    <MousePointer2 className="size-4" /> Click &amp; scroll
                  </CardTitle>
                </CardHeader>
                <CardContent className="space-y-3">
                  <div className="grid grid-cols-3 gap-2">
                    {BUTTONS.map((b) => (
                      <Button
                        key={b.id}
                        variant="secondary"
                        onClick={() =>
                          d.send(`click ${b.id}`, () => api.click(b.id))
                        }
                      >
                        {b.label}
                      </Button>
                    ))}
                  </div>
                  <Separator />
                  <div className="grid grid-cols-2 gap-2">
                    <Button
                      variant="secondary"
                      className="gap-2"
                      onClick={() => d.send("scroll +1", () => api.scroll(1))}
                    >
                      <ArrowUp className="size-4" /> Scroll up
                    </Button>
                    <Button
                      variant="secondary"
                      className="gap-2"
                      onClick={() => d.send("scroll -1", () => api.scroll(-1))}
                    >
                      <ArrowDown className="size-4" /> Scroll down
                    </Button>
                  </div>
                </CardContent>
              </Card>
            </fieldset>
          </TabsContent>

          {/* WATCH */}
          <TabsContent value="watch" className="mt-4">
            <Card>
              <CardHeader className="flex-row items-center justify-between space-y-0 pb-3">
                <div>
                  <CardTitle className="text-sm">Live mouse events</CardTitle>
                  <CardDescription>
                    Streams every passthrough report while watching.
                  </CardDescription>
                </div>
                <div className="flex gap-2">
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={() => d.setEvents([])}
                    className="gap-1.5"
                  >
                    <Trash2 className="size-3.5" /> Clear
                  </Button>
                  <Button
                    size="sm"
                    variant={d.watching ? "destructive" : "default"}
                    disabled={!d.connected}
                    onClick={() => d.toggleWatch(!d.watching)}
                    className="gap-1.5"
                  >
                    {d.watching ? (
                      <>
                        <EyeOff className="size-3.5" /> Stop
                      </>
                    ) : (
                      <>
                        <Eye className="size-3.5" /> Watch
                      </>
                    )}
                  </Button>
                </div>
              </CardHeader>
              <CardContent>
                <ScrollArea className="h-72 rounded-md border">
                  <table className="w-full text-left font-mono text-xs">
                    <thead className="sticky top-0 bg-muted/80 text-muted-foreground backdrop-blur">
                      <tr>
                        <th className="px-3 py-1.5 font-medium">btn</th>
                        <th className="px-3 py-1.5 font-medium">dx</th>
                        <th className="px-3 py-1.5 font-medium">dy</th>
                        <th className="px-3 py-1.5 font-medium">wheel</th>
                      </tr>
                    </thead>
                    <tbody>
                      {d.events.length === 0 ? (
                        <tr>
                          <td
                            colSpan={4}
                            className="px-3 py-8 text-center text-muted-foreground"
                          >
                            {d.watching
                              ? "Move the Superlight…"
                              : "Press Watch to start."}
                          </td>
                        </tr>
                      ) : (
                        d.events
                          .slice()
                          .reverse()
                          .map((e, i) => (
                            <tr key={i} className="border-t">
                              <td className="px-3 py-1 tabular-nums">
                                {e.buttons
                                  ? `0b${e.buttons.toString(2).padStart(5, "0")}`
                                  : "·"}
                              </td>
                              <td className="px-3 py-1 tabular-nums">{e.dx}</td>
                              <td className="px-3 py-1 tabular-nums">{e.dy}</td>
                              <td className="px-3 py-1 tabular-nums">
                                {e.wheel ?? 0}
                              </td>
                            </tr>
                          ))
                      )}
                    </tbody>
                  </table>
                </ScrollArea>
              </CardContent>
            </Card>
          </TabsContent>

          {/* REMAP */}
          <TabsContent value="remap" className="mt-4">
            <Card>
              <CardHeader className="pb-3">
                <CardTitle className="text-sm">Button remap</CardTitle>
                <CardDescription>
                  Rewrites a physical button at the source (the ESP32 intercept).
                  Pick a target button, or <span className="font-medium">Disable</span> to
                  drop it. Keystroke actions (e.g. ctrl+c) need a keyboard HID — coming later.
                </CardDescription>
              </CardHeader>
              <CardContent className="flex flex-wrap items-end gap-3">
                <div className="grid min-w-36 gap-1.5">
                  <Label className="text-xs">From button</Label>
                  <Select value={remapFrom} onValueChange={setRemapFrom}>
                    <SelectTrigger>
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      {BUTTONS.map((b) => (
                        <SelectItem key={b.id} value={b.id}>
                          {b.label}
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </div>
                <div className="grid min-w-36 gap-1.5">
                  <Label className="text-xs">To</Label>
                  <Select value={remapTo} onValueChange={setRemapTo}>
                    <SelectTrigger>
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      {BUTTONS.map((b) => (
                        <SelectItem key={b.id} value={b.id}>
                          {b.label}
                        </SelectItem>
                      ))}
                      <SelectItem value="none">Disable</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                <Button
                  disabled={!d.connected}
                  className="gap-2"
                  onClick={() =>
                    d.send(`remap ${remapFrom}->${remapTo}`, () =>
                      api.setRemap(remapFrom, remapTo),
                    )
                  }
                >
                  <Send className="size-4" /> Apply
                </Button>
              </CardContent>
            </Card>
          </TabsContent>

          {/* CONSOLE */}
          <TabsContent value="console" className="mt-4">
            <Card>
              <CardHeader className="flex-row items-center justify-between space-y-0 pb-3">
                <CardTitle className="flex items-center gap-2 text-sm">
                  <Terminal className="size-4" /> Console
                </CardTitle>
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={() => d.setLog([])}
                  className="gap-1.5"
                >
                  <Trash2 className="size-3.5" /> Clear
                </Button>
              </CardHeader>
              <CardContent>
                <ScrollArea className="h-72 rounded-md border bg-muted/30 p-3">
                  <div className="space-y-0.5 font-mono text-xs">
                    {d.log.length === 0 ? (
                      <p className="text-muted-foreground">No traffic yet.</p>
                    ) : (
                      d.log.map((l) => (
                        <div key={l.id} className="flex gap-2">
                          <span
                            className={cn(
                              "shrink-0 select-none",
                              l.dir === "rx" && "text-success",
                              l.dir === "tx" && "text-primary",
                              l.dir === "sys" && "text-muted-foreground",
                            )}
                          >
                            {l.dir === "rx" ? "←" : l.dir === "tx" ? "→" : "•"}
                          </span>
                          <span className="break-all">{l.text}</span>
                        </div>
                      ))
                    )}
                  </div>
                </ScrollArea>
              </CardContent>
            </Card>
          </TabsContent>
        </Tabs>
      </main>
    </div>
  );
}
