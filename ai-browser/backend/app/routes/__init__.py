from fastapi import APIRouter
from .search import router as search_router
from .index import router as index_router

router = APIRouter()
router.include_router(search_router)
router.include_router(index_router)
