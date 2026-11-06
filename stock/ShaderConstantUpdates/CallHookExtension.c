/* Local extension: redirect a complete E8 instruction without moving its callee.
 * Included after MinHook hook.c; the original license remains in that file. */
MH_STATUS WINAPI MH_CreateCallHook(LPVOID target, LPVOID destination)
{
    MH_STATUS status = MH_OK;
    EnterSpinLock();
    if (!g_hHeap) status = MH_ERROR_NOT_INITIALIZED;
    else if (!IsExecutableAddress(target) || !IsExecutableAddress(destination) || *(LPBYTE)target != 0xE8)
        status = MH_ERROR_UNSUPPORTED_FUNCTION;
    else if (FindHookEntry(target) != INVALID_HOOK_POS) status = MH_ERROR_ALREADY_CREATED;
    else
    {
        INT64 delta = (LPBYTE)destination - ((LPBYTE)target + 5);
        if (delta < INT_MIN || delta > INT_MAX) status = MH_ERROR_UNSUPPORTED_FUNCTION;
        else
        {
            LPVOID spare = AllocateBuffer(target);
            PHOOK_ENTRY entry = spare ? AddHookEntry() : NULL;
            if (!entry) { if (spare) FreeBuffer(spare); status = MH_ERROR_MEMORY_ALLOC; }
            else
            {
                ZeroMemory(entry, sizeof(*entry));
                entry->pTarget = target; entry->pDetour = destination; entry->pTrampoline = spare;
                entry->isCall = TRUE;
                memcpy(entry->backup, target, 5);
            }
        }
    }
    LeaveSpinLock(); return status;
}
MH_STATUS WINAPI MH_SetCallHookDestination(LPVOID target, LPVOID destination)
{
    MH_STATUS status = MH_OK;
    EnterSpinLock();
    UINT pos = FindHookEntry(target);
    INT64 delta = (LPBYTE)destination - ((LPBYTE)target + 5);
    if (pos == INVALID_HOOK_POS || !g_hooks.pItems[pos].isCall) status = MH_ERROR_NOT_CREATED;
    else if (g_hooks.pItems[pos].isEnabled) status = MH_ERROR_ENABLED;
    else if (delta < INT_MIN || delta > INT_MAX || !IsExecutableAddress(destination)) status = MH_ERROR_UNSUPPORTED_FUNCTION;
    else g_hooks.pItems[pos].pDetour = destination;
    LeaveSpinLock(); return status;
}
