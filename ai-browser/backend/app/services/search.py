import os
import httpx
from tenacity import retry, stop_after_attempt, wait_exponential, retry_if_exception_type
from typing import List, Dict

SEARXNG_URL = os.getenv("SEARXNG_URL", "http://localhost:8888")
GOOGLE_API_KEY = os.getenv("GOOGLE_API_KEY")
GOOGLE_CX = os.getenv("GOOGLE_CX")

@retry(stop=stop_after_attempt(2), wait=wait_exponential(min=1, max=4), retry=retry_if_exception_type(httpx.HTTPError))
async def search_searxng(query: str, limit: int = 10, engines: str | None = None) -> List[Dict]:
    params: Dict[str, str] = {"q": query, "format": "json"}
    if engines:
        params["engines"] = engines
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(
                f"{SEARXNG_URL}/search",
                params=params,
            )
            resp.raise_for_status()
        except httpx.HTTPError:
            return []
        results = resp.json()
        return [
            {
                "title": r.get("title", ""),
                "url": r.get("url", ""),
                "snippet": r.get("content", ""),
            }
            for r in results.get("results", [])[:limit]
        ]

@retry(stop=stop_after_attempt(2), wait=wait_exponential(min=1, max=4), retry=retry_if_exception_type(httpx.HTTPError))
async def search_google_cse(query: str, limit: int = 10) -> List[Dict]:
    if not GOOGLE_API_KEY or not GOOGLE_CX:
        return []
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(
                "https://www.googleapis.com/customsearch/v1",
                params={"key": GOOGLE_API_KEY, "cx": GOOGLE_CX, "q": query, "num": limit},
            )
            resp.raise_for_status()
        except httpx.HTTPError:
            return []
        items = resp.json().get("items", [])
        return [
            {
                "title": i.get("title", ""),
                "url": i.get("link", ""),
                "snippet": i.get("snippet", ""),
            }
            for i in items
        ]

async def search_web(query: str, ai_overview: bool = True, engine: str | None = None) -> Dict:
    results: List[Dict] = []
    if engine == "google":
        results = await search_google_cse(query)
    elif engine == "duckduckgo":
        results = await search_searxng(query, engines="duckduckgo")
    elif engine == "bing":
        results = await search_searxng(query, engines="bing")
    else:
        results = await search_searxng(query)
        if not results:
            results = await search_google_cse(query)
    return {"query": query, "results": results}
