import { useState, useCallback } from "react";

const API_BASE = "http://127.0.0.1:8000/api/v1";

type IndexResult = { id?: string; title?: string; url?: string; snippet?: string };

export function useIndexSearch() {
  const [loading, setLoading] = useState(false);
  const [results, setResults] = useState<IndexResult[]>([]);
  const [error, setError] = useState<string | null>(null);

  const search = useCallback(async (query: string) => {
    setLoading(true);
    setError(null);
    try {
      const params = new URLSearchParams({ q: query, limit: "10" });
      const resp = await fetch(`${API_BASE}/index/search?${params}`);
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const data = await resp.json();
      setResults(data.results || []);
    } catch (err) {
      console.error("Index search failed:", err);
      setResults([]);
      setError(err instanceof Error ? err.message : "Index search failed");
    } finally {
      setLoading(false);
    }
  }, []);

  return { search, loading, results, error };
}
