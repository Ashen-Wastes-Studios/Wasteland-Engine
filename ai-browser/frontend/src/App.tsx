import { useState, useCallback, useEffect } from "react";
import { SplitPane } from "./components/SplitPane";
import { TabBar } from "./components/TabBar";
import { CommandPalette } from "./components/CommandPalette";
import { WebView } from "./components/WebView";
import { useSearch } from "./hooks/useSearch";

type Tab = { id: string; title: string; url: string };

export default function App() {
  const [tabs, setTabs] = useState<Tab[]>([{ id: "1", title: "New Tab", url: "about:blank" }]);
  const [active, setActive] = useState("1");
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [cmdOpen, setCmdOpen] = useState(false);
  const [url, setUrl] = useState("");
  const [mode, setMode] = useState<"web" | "search">("web");
  const [error, setError] = useState<string | null>(null);
  const { search, loading, overview, results, error: searchError, setError: setSearchError } = useSearch();

  const addTab = useCallback(() => {
    const id = crypto.randomUUID();
    setTabs((t) => [...t, { id, title: "New Tab", url: "about:blank" }]);
    setActive(id);
  }, []);

  const closeTab = useCallback(
    (id: string) => {
      setTabs((t) => {
        const next = t.filter((tab) => tab.id !== id);
        if (next.length === 0) return [{ id: "1", title: "New Tab", url: "about:blank" }];
        if (active === id) setActive(next[0].id);
        return next;
      });
    },
    [active],
  );

  const navigate = useCallback((u: string) => {
    setUrl(u);
    setMode("web");
    setError(null);
    setSearchError(null);
    setTabs((t) =>
      t.map((tab) =>
        tab.id === active
          ? { ...tab, url: u, title: u.replace(/^https?:\/\//, "").split("/")[0] || u }
          : tab,
      ),
    );
  }, [active, setSearchError]);

  const handleCommand = useCallback(
    (cmd: string) => {
      if (cmd === "tab.new") addTab();
      if (cmd === "sidebar.toggle") setSidebarOpen((o) => !o);
      if (cmd === "ai.toggle") {
        // TODO: toggle AI overview panel
      }
    },
    [addTab],
  );

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key === "p") {
        e.preventDefault();
        setCmdOpen((o) => !o);
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, []);

  const handleSubmit = () => {
    const trimmed = url.trim();
    if (!trimmed) return;
    setError(null);
    setSearchError(null);
    if (/^(https?:\/\/)/.test(trimmed) || (trimmed.includes(".") && !trimmed.includes(" "))) {
      navigate(trimmed);
    } else {
      setMode("search");
      search(trimmed);
    }
  };

  const activeTab = tabs.find((t) => t.id === active) || tabs[0];

  return (
    <div
      style={{
        height: "100vh",
        display: "flex",
        flexDirection: "column",
        background: "var(--card)",
        color: "var(--foreground)",
      }}
    >
      <TabBar tabs={tabs} active={active} onSelect={setActive} onClose={closeTab} onNewTab={addTab} />
      <div style={{ display: "flex", flex: 1, minHeight: 0 }}>
        {sidebarOpen && (
          <aside
            style={{
              width: 240,
              borderRight: "1px solid var(--border)",
              padding: 8,
              overflow: "auto",
            }}
          >
            <div style={{ fontWeight: 600, marginBottom: 8 }}>History</div>
            <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
            <div style={{ fontWeight: 600, margin: "16px 0 8px" }}>Bookmarks</div>
            <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
            <div style={{ fontWeight: 600, margin: "16px 0 8px" }}>Indexed Pages</div>
            <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
          </aside>
        )}
        <main style={{ flex: 1, display: "flex", flexDirection: "column", minWidth: 0 }}>
          <div
            style={{
              display: "flex",
              gap: 8,
              padding: 8,
              borderBottom: "1px solid var(--border)",
            }}
          >
            <input
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === "Enter") handleSubmit();
              }}
              placeholder="Search or enter URL"
              style={{
                flex: 1,
                padding: "6px 10px",
                background: "var(--border)",
                color: "var(--foreground)",
                border: "1px solid var(--border)",
                borderRadius: 4,
                outline: "none",
              }}
            />
            <button
              onClick={handleSubmit}
              style={{
                padding: "6px 12px",
                background: "var(--accent)",
                border: "none",
                color: "#fff",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              Go
            </button>
          </div>
          <div style={{ flex: 1, minHeight: 0, position: "relative" }}>
            {(searchError || error) && (
              <div style={{ padding: 12, background: "#3a0f0f", color: "#ffb3b3", borderBottom: "1px solid var(--border)" }}>
                {searchError || error}
              </div>
            )}
            {mode === "search" ? (
              <SplitPane
                direction="vertical"
                defaultLeft={220}
                left={
                  <div style={{ padding: 12, overflow: "auto" }}>
                    <h3 style={{ marginBottom: 8 }}>AI Overview</h3>
                    {loading ? (
                      <div style={{ display: "flex", gap: 6, flexWrap: "wrap" }}>
                        {[0, 1, 2].map((i) => (
                          <div
                            key={i}
                            style={{
                              height: 10,
                              flex: 1,
                              background: "var(--border)",
                              borderRadius: 4,
                              animation: "pulse 1.5s infinite ease-in-out",
                              animationDelay: `${i * 150}ms`,
                            }}
                          />
                        ))}
                      </div>
                    ) : overview ? (
                      <p style={{ fontSize: 13, lineHeight: 1.5 }}>{overview}</p>
                    ) : (
                      <p style={{ fontSize: 13, color: "var(--muted-foreground)" }}>
                        No overview available.
                      </p>
                    )}
                    <h3 style={{ margin: "16px 0 8px" }}>Results</h3>
                    {results.length === 0 && !loading && (
                      <p style={{ fontSize: 12, color: "var(--muted-foreground)" }}>No results.</p>
                    )}
                    {results.map((r, i) => (
                      <div key={i} style={{ marginBottom: 12 }}>
                        <a
                          href={r.url}
                          target="_blank"
                          rel="noreferrer"
                          style={{ color: "var(--accent)", fontSize: 13, textDecoration: "none" }}
                        >
                          {r.title}
                        </a>
                        <p style={{ fontSize: 12, color: "var(--muted-foreground)", marginTop: 4 }}>
                          {r.snippet}
                        </p>
                      </div>
                    ))}
                  </div>
                }
                right={<WebView url={activeTab.url} />}
              />
            ) : (
              <WebView url={activeTab.url} />
            )}
          </div>
        </main>
      </div>
      <CommandPalette open={cmdOpen} onClose={() => setCmdOpen(false)} onSelect={handleCommand} />
    </div>
  );
}
