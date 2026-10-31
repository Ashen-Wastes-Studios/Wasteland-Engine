import { useState, useCallback, useEffect } from "react";
import { SplitPane } from "./components/SplitPane";
import { TabBar } from "./components/TabBar";
import { CommandPalette } from "./components/CommandPalette";
import { WebView } from "./components/WebView";
import { DownloadsPanel } from "./components/DownloadsPanel";
import { ExtensionsPanel } from "./components/ExtensionsPanel";
import { PasswordsPanel } from "./components/PasswordsPanel";
import { TabGroupsPanel } from "./components/TabGroupsPanel";
import { useSearch } from "./hooks/useSearch";
import { useSettings } from "./hooks/useSettings";
import { useDownloads } from "./hooks/useDownloads";
import { useExtensions } from "./hooks/useExtensions";
import { usePasswords } from "./hooks/usePasswords";
import type { TabGroup } from "./extensions";
import type { Theme, SearchEngine } from "./types";

type Tab = { id: string; title: string; url: string; history: string[]; historyIndex: number; groupId?: string };
type HistoryEntry = { url: string; title: string; timestamp: number };
type Bookmark = { url: string; title: string };

const STORAGE_KEYS = {
  history: "ai-browser-history",
  bookmarks: "ai-browser-bookmarks",
  settings: "ai-browser-settings",
} as const;

const navStyle: Record<string, string | number> = {
  padding: "4px 8px",
  background: "transparent",
  border: "1px solid var(--border)",
  color: "var(--foreground)",
  cursor: "pointer",
  borderRadius: 4,
  fontSize: 14,
  lineHeight: 1,
};

function loadStorage<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

export default function App() {
  const [tabs, setTabs] = useState<Tab[]>([{ id: "1", title: "New Tab", url: "about:blank", history: [], historyIndex: -1 }]);
  const [active, setActive] = useState("1");
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [cmdOpen, setCmdOpen] = useState(false);
  const [url, setUrl] = useState("");
  const [mode, setMode] = useState<"web" | "search">("web");
  const [history, setHistory] = useState<HistoryEntry[]>(() => loadStorage(STORAGE_KEYS.history, []));
  const [bookmarks, setBookmarks] = useState<Bookmark[]>(() => loadStorage(STORAGE_KEYS.bookmarks, []));
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [downloadsOpen, setDownloadsOpen] = useState(false);
  const [extensionsOpen, setExtensionsOpen] = useState(false);
  const [tabGroupsOpen, setTabGroupsOpen] = useState(false);
  const [passwordsOpen, setPasswordsOpen] = useState(false);
  const [incognito, setIncognito] = useState(false);
  const [tabGroups, setTabGroups] = useState<TabGroup[]>([]);
  const { settings, update: updateSettings } = useSettings();
  const { downloads, clearCompleted } = useDownloads();
  const { extensions, toggleExtension, removeExtension } = useExtensions();
  const { passwords, removePassword } = usePasswords();
  const { search, loading, overview, results, error: searchError, setError: setSearchError } = useSearch();

  useEffect(() => {
    const root = document.documentElement;
    root.setAttribute("data-theme", settings.theme);
  }, [settings.theme]);

  const addTab = useCallback(() => {
    const id = crypto.randomUUID();
    setTabs((t) => [...t, { id, title: "New Tab", url: "about:blank", history: [], historyIndex: -1 }]);
    setActive(id);
  }, []);

  const closeTab = useCallback(
    (id: string) => {
      setTabs((t) => {
        const next = t.filter((tab) => tab.id !== id);
        if (next.length === 0) return [{ id: "1", title: "New Tab", url: "about:blank", history: [], historyIndex: -1 }];
        if (active === id) setActive(next[0].id);
        return next;
      });
    },
    [active],
  );

  const navigate = useCallback((u: string) => {
    setUrl(u);
    setMode("web");
    setSearchError(null);
    const title = u.replace(/^https?:\/\//, "").split("/")[0] || u;
    setTabs((t) =>
      t.map((tab) => {
        if (tab.id !== active) return tab;
        const history = tab.history.slice(0, tab.historyIndex + 1);
        return {
          ...tab,
          url: u,
          title,
          history: [...history, u],
          historyIndex: history.length,
        };
      }),
    );
    setHistory((h) => {
      const filtered = h.filter((entry) => entry.url !== u);
      return [{ url: u, title, timestamp: Date.now() }, ...filtered].slice(0, 200);
    });
  }, [active, setSearchError]);

  const goBack = useCallback(() => {
    setTabs((t) =>
      t.map((tab) => {
        if (tab.id !== active || tab.historyIndex <= 0) return tab;
        const newIndex = tab.historyIndex - 1;
        const url = tab.history[newIndex];
        return { ...tab, url, historyIndex: newIndex };
      }),
    );
  }, [active]);

  const goForward = useCallback(() => {
    setTabs((t) =>
      t.map((tab) => {
        if (tab.id !== active || tab.historyIndex >= tab.history.length - 1) return tab;
        const newIndex = tab.historyIndex + 1;
        const url = tab.history[newIndex];
        return { ...tab, url, historyIndex: newIndex };
      }),
    );
  }, [active]);

  const refresh = useCallback(() => {
    setTabs((t) => t.map((tab) => (tab.id === active ? { ...tab } : tab)));
  }, [active]);

  const goHome = useCallback(() => {
    navigate("about:blank");
  }, [navigate]);

  const handleCommand = useCallback(
    (cmd: string) => {
      if (cmd === "tab.new") addTab();
      if (cmd === "sidebar.toggle") setSidebarOpen((o) => !o);
      if (cmd === "settings.open") setSettingsOpen((o) => !o);
      if (cmd === "downloads.open") setDownloadsOpen((o) => !o);
      if (cmd === "extensions.open") setExtensionsOpen((o) => !o);
      if (cmd === "tabgroups.open") setTabGroupsOpen((o) => !o);
      if (cmd === "passwords.open") setPasswordsOpen((o) => !o);
      if (cmd === "bookmark.add") {
        const current = tabs.find((t) => t.id === active);
        if (current && current.url !== "about:blank") {
          setBookmarks((b) => {
            const exists = b.some((item) => item.url === current.url);
            if (exists) return b.filter((item) => item.url !== current.url);
            return [...b, { url: current.url, title: current.title }];
          });
        }
      }
      if (cmd === "ai.toggle") {
        // TODO: toggle AI overview panel
      }
    },
    [addTab, active, tabs],
  );

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key === "p") {
        e.preventDefault();
        setCmdOpen((o) => !o);
      }
      if ((e.ctrlKey || e.metaKey) && e.key === "d") {
        e.preventDefault();
        handleCommand("bookmark.add");
      }
      if ((e.ctrlKey || e.metaKey) && e.key === "n") {
        e.preventDefault();
        setIncognito((o) => !o);
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [handleCommand]);

  useEffect(() => {
    const current = tabs.find((t) => t.id === active);
    if (current && current.url !== url) {
      setUrl(current.url);
    }
  }, [active, tabs, url]);

  useEffect(() => {
    localStorage.setItem(STORAGE_KEYS.history, JSON.stringify(history.slice(0, 200)));
  }, [history]);

  useEffect(() => {
    localStorage.setItem(STORAGE_KEYS.bookmarks, JSON.stringify(bookmarks));
  }, [bookmarks]);

  const handleSubmit = () => {
    const trimmed = url.trim();
    if (!trimmed) return;
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
            {downloadsOpen ? (
              <DownloadsPanel downloads={downloads} onClear={clearCompleted} />
            ) : extensionsOpen ? (
              <ExtensionsPanel
                extensions={extensions}
                onToggle={toggleExtension}
                onRemove={removeExtension}
              />
            ) : tabGroupsOpen ? (
              <TabGroupsPanel
                groups={tabGroups}
                onSelect={(id) => {
                  setTabGroups((g) => g.filter((group) => group.id !== id));
                  setTabGroupsOpen(false);
                }}
                onAdd={() => {
                  const colors = ["#dc143c", "#ff7f50", "#00ced1", "#32cd32", "#ffd700"];
                  const color = colors[tabGroups.length % colors.length];
                  const name = `Group ${tabGroups.length + 1}`;
                  setTabGroups((prev) => [...prev, { id: crypto.randomUUID(), name, color }]);
                }}
              />
            ) : passwordsOpen ? (
              <PasswordsPanel passwords={passwords} onRemove={removePassword} />
            ) : (
              <>
                <div style={{ fontWeight: 600, marginBottom: 8 }}>History</div>
                {history.length === 0 && (
                  <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
                )}
                {history.map((entry, i) => (
                  <div
                    key={`${entry.url}-${entry.timestamp}-${i}`}
                    onClick={() => navigate(entry.url)}
                    style={{
                      fontSize: 12,
                      padding: "4px 6px",
                      cursor: "pointer",
                      borderRadius: 4,
                      whiteSpace: "nowrap",
                      overflow: "hidden",
                      textOverflow: "ellipsis",
                    }}
                    title={entry.url}
                  >
                    {entry.title || entry.url}
                  </div>
                ))}
                <div style={{ fontWeight: 600, margin: "16px 0 8px" }}>Bookmarks</div>
                {bookmarks.length === 0 && (
                  <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
                )}
                {bookmarks.map((bookmark, i) => (
                  <div
                    key={`${bookmark.url}-${i}`}
                    onClick={() => navigate(bookmark.url)}
                    style={{
                      fontSize: 12,
                      padding: "4px 6px",
                      cursor: "pointer",
                      borderRadius: 4,
                      whiteSpace: "nowrap",
                      overflow: "hidden",
                      textOverflow: "ellipsis",
                    }}
                    title={bookmark.url}
                  >
                    {bookmark.title || bookmark.url}
                  </div>
                ))}
                <div style={{ fontWeight: 600, margin: "16px 0 8px" }}>Indexed Pages</div>
                <div style={{ fontSize: 12, color: "var(--muted-foreground)" }}>Empty</div>
              </>
            )}
          </aside>
        )}
        <main style={{ flex: 1, display: "flex", flexDirection: "column", minWidth: 0 }}>
          <div
            style={{
              display: "flex",
              gap: 4,
              padding: 8,
              borderBottom: "1px solid var(--border)",
              alignItems: "center",
            }}
          >
            <button onClick={goBack} title="Back" style={navStyle}>←</button>
            <button onClick={goForward} title="Forward" style={navStyle}>→</button>
            <button onClick={refresh} title="Refresh" style={navStyle}>↻</button>
            <button onClick={goHome} title="Home" style={navStyle}>🏠</button>
            <input
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === "Enter") handleSubmit();
              }}
              placeholder={incognito ? "Incognito - Search or enter URL" : "Search or enter URL"}
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
            <button
              onClick={() => handleCommand("bookmark.add")}
              title="Bookmark (Ctrl+D)"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              ★
            </button>
            <button
              onClick={() => setSettingsOpen((o) => !o)}
              title="Settings"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              ⚙
            </button>
            <button
              onClick={() => setDownloadsOpen((o) => !o)}
              title="Downloads"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              ↓
            </button>
            <button
              onClick={() => setExtensionsOpen((o) => !o)}
              title="Extensions"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              🧩
            </button>
            <button
              onClick={() => setTabGroupsOpen((o) => !o)}
              title="Tab Groups"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              📑
            </button>
            <button
              onClick={() => setPasswordsOpen((o) => !o)}
              title="Passwords"
              style={{
                padding: "6px 10px",
                background: "transparent",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              🔒
            </button>
            <button
              onClick={() => setIncognito((o) => !o)}
              title={incognito ? "Exit Incognito" : "Incognito Mode"}
              style={{
                padding: "6px 10px",
                background: incognito ? "var(--accent)" : "transparent",
                border: "1px solid var(--border)",
                color: "#fff",
                cursor: "pointer",
                borderRadius: 4,
              }}
            >
              {incognito ? "🕶" : "Incognito"}
            </button>
          </div>
          {bookmarks.length > 0 && !incognito && (
            <div
              style={{
                display: "flex",
                gap: 6,
                padding: "4px 8px",
                borderBottom: "1px solid var(--border)",
                overflowX: "auto",
                alignItems: "center",
              }}
            >
              {bookmarks.map((bookmark, i) => (
                <button
                  key={`${bookmark.url}-${i}`}
                  onClick={() => navigate(bookmark.url)}
                  title={bookmark.url}
                  style={{
                    background: "transparent",
                    border: "none",
                    color: "var(--foreground)",
                    cursor: "pointer",
                    fontSize: 12,
                    padding: "2px 4px",
                    borderRadius: 4,
                    whiteSpace: "nowrap",
                  }}
                >
                  {bookmark.title || bookmark.url}
                </button>
              ))}
            </div>
          )}
          {settingsOpen && (
            <div
              style={{
                padding: 12,
                borderBottom: "1px solid var(--border)",
                display: "flex",
                gap: 12,
                flexWrap: "wrap",
                alignItems: "center",
              }}
            >
              <label style={{ fontSize: 12 }}>
                SearXNG URL:
                <input
                  value={settings.searxngUrl}
                  onChange={(e) => updateSettings({ searxngUrl: e.target.value })}
                  style={{ marginLeft: 8, padding: "4px 6px", background: "var(--border)", color: "var(--foreground)", border: "1px solid var(--border)", borderRadius: 4 }}
                />
              </label>
              <label style={{ fontSize: 12 }}>
                LLM URL:
                <input
                  value={settings.llmUrl}
                  onChange={(e) => updateSettings({ llmUrl: e.target.value })}
                  style={{ marginLeft: 8, padding: "4px 6px", background: "var(--border)", color: "var(--foreground)", border: "1px solid var(--border)", borderRadius: 4 }}
                />
              </label>
              <label style={{ fontSize: 12 }}>
                LLM Model:
                <input
                  value={settings.llmModel}
                  onChange={(e) => updateSettings({ llmModel: e.target.value })}
                  style={{ marginLeft: 8, padding: "4px 6px", background: "var(--border)", color: "var(--foreground)", border: "1px solid var(--border)", borderRadius: 4 }}
                />
              </label>
              <label style={{ fontSize: 12 }}>
                Theme:
                <select
                  value={settings.theme}
                  onChange={(e) => updateSettings({ theme: e.target.value as Theme })}
                  style={{ marginLeft: 8, padding: "4px 6px", background: "var(--border)", color: "var(--foreground)", border: "1px solid var(--border)", borderRadius: 4 }}
                >
                  <option value="dark">Dark</option>
                  <option value="light">Light</option>
                </select>
              </label>
              <label style={{ fontSize: 12 }}>
                Search engine:
                <select
                  value={settings.searchEngine}
                  onChange={(e) => updateSettings({ searchEngine: e.target.value as SearchEngine })}
                  style={{ marginLeft: 8, padding: "4px 6px", background: "var(--border)", color: "var(--foreground)", border: "1px solid var(--border)", borderRadius: 4 }}
                >
                  <option value="searxng">SearXNG</option>
                  <option value="google">Google</option>
                  <option value="duckduckgo">DuckDuckGo</option>
                  <option value="bing">Bing</option>
                </select>
              </label>
            </div>
          )}
          <div style={{ flex: 1, minHeight: 0, position: "relative" }}>
            {searchError && (
              <div style={{ padding: 12, background: "#3a0f0f", color: "#ffb3b3", borderBottom: "1px solid var(--border)" }}>
                {searchError}
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
