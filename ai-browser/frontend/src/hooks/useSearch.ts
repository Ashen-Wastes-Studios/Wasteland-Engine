import { useState, useCallback } from "react";

const API_BASE = "http://127.0.0.1:8000/api/v1";

type SearchResult = { title: string; url: string; snippet: string };

export function useSearch() {
  const [loading, setLoading] = useState(false);
  const [overview, setOverview] = useState<string | null>(null);
  const [results, setResults] = useState<SearchResult[]>([]);
  const [error, setError] = useState<string | null>(null);

  const search = useCallback(async (query: string) => {
    setLoading(true);
    setError(null);
    try {
      const params = new URLSearchParams({ q: query, ai_overview: "true" });
      const resp = await fetch(`${API_BASE}/search?${params}`);
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const data = await resp.json();
      setOverview(data.ai_overview);
      setResults(data.results || []);
    } catch (err) {
      console.error("Search failed:", err);
      setOverview(null);
      setResults([]);
      setError(err instanceof Error ? err.message : "Search failed");
    } finally {
      setLoading(false);
    }
  }, []);

  return { search, loading, overview, results, error, setError };
}
