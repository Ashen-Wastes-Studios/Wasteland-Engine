import os
import meilisearch
from typing import List, Dict

MEILI_URL = os.getenv("MEILI_URL", "http://localhost:7700")
MEILI_KEY = os.getenv("MEILI_KEY", "masterKey")
INDEX_NAME = os.getenv("MEILI_INDEX", "webpages")

_client = meilisearch.Client(MEILI_URL, MEILI_KEY)
_index = _client.index(INDEX_NAME)

async def index_document(doc: Dict):
    _index.add_documents([doc], primary_key="id")

async def search_index(query: str, limit: int = 10) -> List[Dict]:
    resp = _index.search(query, {"limit": limit})
    return resp.get("hits", [])
