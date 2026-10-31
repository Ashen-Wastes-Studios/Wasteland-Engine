import os
import asyncio
import hashlib
import httpx
from typing import List, Dict, Optional
from html.parser import HTMLParser

class TextExtractor(HTMLParser):
    def __init__(self):
        super().__init__()
        self.text_parts: List[str] = []
        self.skip = False
        self.skip_tags = {"script", "style", "nav", "footer", "header"}

    def handle_starttag(self, tag, attrs):
        if tag in self.skip_tags:
            self.skip = True

    def handle_endtag(self, tag):
        if tag in self.skip_tags:
            self.skip = False

    def handle_data(self, data):
        if not self.skip:
            stripped = data.strip()
            if stripped:
                self.text_parts.append(stripped)

def extract_text(html: str) -> str:
    parser = TextExtractor()
    parser.feed(html)
    return " ".join(parser.text_parts)

async def crawl_url(url: str, max_depth: int = 1) -> Dict:
    async with httpx.AsyncClient(timeout=20.0, follow_redirects=True) as client:
        resp = await client.get(url)
        resp.raise_for_status()
        text = extract_text(resp.text)
        return {
            "id": hashlib.sha256(url.encode()).hexdigest(),
            "url": str(resp.url),
            "title": "",  # TODO: parse <title>
            "content": text[:100_000],
        }
