import { useEffect, useRef, useState } from "react";

export function WebView({ url }: { url: string }) {
  const iframeRef = useRef<HTMLIFrameElement>(null);
  const [loading, setLoading] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);

  useEffect(() => {
    if (!url || url === "about:blank") {
      setLoading(false);
      setLoadError(null);
      return;
    }

    setLoading(true);
    setLoadError(null);

    const timer = setTimeout(() => {
      setLoading(false);
    }, 4000);

    const handleLoad = () => {
      setLoading(false);
      setLoadError(null);
    };

    const handleError = () => {
      setLoading(false);
      setLoadError("This page could not be loaded in the embedded view.");
    };

    const iframe = iframeRef.current;
    if (iframe) {
      iframe.src = url;
      iframe.addEventListener("load", handleLoad);
      iframe.addEventListener("error", handleError);
    }

    return () => {
      clearTimeout(timer);
      if (iframe) {
        iframe.removeEventListener("load", handleLoad);
        iframe.removeEventListener("error", handleError);
      }
    };
  }, [url]);

  if (url === "about:blank") {
    return (
      <div style={{ padding: 24, opacity: 0.7 }}>
        <h2>AI Browser Shell</h2>
        <p>Start by searching or entering a URL.</p>
      </div>
    );
  }

  return (
    <div style={{ position: "relative", width: "100%", height: "100%" }}>
      {loading && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            background: "var(--card)",
            color: "var(--muted-foreground)",
            fontSize: 13,
            zIndex: 2,
          }}
        >
          Loading {url}...
        </div>
      )}
      {loadError && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            background: "var(--card)",
            color: "#ffb3b3",
            fontSize: 13,
            padding: 24,
            textAlign: "center",
            zIndex: 2,
          }}
        >
          <div>
            <div style={{ marginBottom: 8, fontWeight: 600 }}>Webview load failed</div>
            <div>{loadError}</div>
            <div style={{ marginTop: 8 }}>
              <a href={url} target="_blank" rel="noreferrer" style={{ color: "var(--accent)" }}>
                Open in external browser
              </a>
            </div>
          </div>
        </div>
      )}
      <iframe
        ref={iframeRef}
        title="webview"
        sandbox="allow-scripts allow-same-origin allow-forms allow-popups"
        style={{ width: "100%", height: "100%", border: "none" }}
      />
    </div>
  );
}

