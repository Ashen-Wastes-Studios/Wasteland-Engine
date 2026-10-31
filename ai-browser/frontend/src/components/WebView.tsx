import { useEffect, useRef } from "react";

export function WebView({ url }: { url: string }) {
  const iframeRef = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    if (iframeRef.current) {
      iframeRef.current.src = url;
    }
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
    <iframe
      ref={iframeRef}
      title="webview"
      sandbox="allow-scripts allow-same-origin allow-forms allow-popups"
      style={{ width: "100%", height: "100%", border: "none" }}
    />
  );
}
