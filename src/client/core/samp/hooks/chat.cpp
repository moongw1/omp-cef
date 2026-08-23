#include "chat.hpp"

#include <cstdint>
#include <cstring>

#include "browser/focus.hpp"
#include "browser/manager.hpp"
#include "hooks/hook_manager.hpp"
#include "network/network_manager.hpp"
#include "samp/addresses.hpp"
#include "system/logger.hpp"
#include "shared/events.hpp"
#include "utf8.hpp"

namespace
{
    // CChat is packed in the supported 32-bit SA:MP builds. In R1/R3/R5 the
    // m_pScrollbar pointer lives at offset 0x11E.
    constexpr std::size_t kChatScrollbarOffset = 0x11E;

    bool IsWritableAddress(const void* address, std::size_t size)
    {
        if (!address || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
            return false;

        const DWORD protection = mbi.Protect & 0xFF;
        if (protection == PAGE_NOACCESS || protection == PAGE_GUARD || protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_READONLY)
            return false;

        const auto start = reinterpret_cast<std::uintptr_t>(address);
        const auto end = start + size;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return end <= regionEnd;
    }

    void HideNativeChatScrollbar(void* chat)
    {
        if (!chat || sizeof(void*) != 4)
            return;

        void* scrollbar = nullptr;
        const auto* scrollbarSlot = static_cast<const std::uint8_t*>(chat) + kChatScrollbarOffset;

        // CChat is packed, so use memcpy instead of an unaligned pointer load.
        std::memcpy(&scrollbar, scrollbarSlot, sizeof(scrollbar));
        if (!scrollbar)
            return;

        // Legacy DXUT's CDXUTControl has a vtable pointer first and m_bVisible
        // immediately after it. CDXUTScrollBar derives directly from it.
        auto* visible = static_cast<std::uint8_t*>(scrollbar) + sizeof(void*);
        if (IsWritableAddress(visible, sizeof(bool)))
            *reinterpret_cast<bool*>(visible) = false;
    }

    bool HasCefChatInputController(const BrowserManager& browserManager)
    {
        for (const auto& [id, holder] : browserManager.GetAllBrowsers())
        {
            (void)id;
            if (!holder)
                continue;

            const auto& instance = *holder;
            if (instance.closing)
                continue;

            // Only overlay browsers explicitly created with controls_chat=true
            // are allowed to activate the SA:MP chat-input bridge.
            if (instance.mode == RenderMode::Overlay2D && instance.controls_chat_input)
                return true;
        }

        return false;
    }
}

bool ChatHook::Initialize()
{
    s_self_ = this;

    auto& addrs = SampAddresses::Instance();
    auto* base = addrs.Base();

    void* addrOpenChat = nullptr;
    void* addrCloseChat = nullptr;
    void* addrRenderChat = nullptr;
    void* addrDrawChat = nullptr;
    void* addrRenderChatToSurface = nullptr;

    switch (addrs.Version())
    {
        case SampVersion::V037:
            addrOpenChat = reinterpret_cast<void*>(base + 0x657E0);
            addrCloseChat = reinterpret_cast<void*>(base + 0x658E0);
            addrRenderChat = reinterpret_cast<void*>(base + 0x63D70);
            addrDrawChat = reinterpret_cast<void*>(base + 0x64230);
            addrRenderChatToSurface = reinterpret_cast<void*>(base + 0x64300);
            break;
        case SampVersion::V037R3:
            addrOpenChat = reinterpret_cast<void*>(base + 0x68D10);
            addrCloseChat = reinterpret_cast<void*>(base + 0x68E10);
            addrRenderChat = reinterpret_cast<void*>(base + 0x671C0);
            addrDrawChat = reinterpret_cast<void*>(base + 0x67680);
            addrRenderChatToSurface = reinterpret_cast<void*>(base + 0x67750);
            break;
        case SampVersion::V037R5:
            addrOpenChat = reinterpret_cast<void*>(base + 0x69480);
            addrCloseChat = reinterpret_cast<void*>(base + 0x69580);
            addrRenderChat = reinterpret_cast<void*>(base + 0x67940);
            addrDrawChat = reinterpret_cast<void*>(base + 0x67E00);
            addrRenderChatToSurface = reinterpret_cast<void*>(base + 0x67ED0);
            break;
        case SampVersion::V03DLR1:
            addrOpenChat = reinterpret_cast<void*>(base + 0x68EC0);
            addrCloseChat = reinterpret_cast<void*>(base + 0x68FC0);
            // Native chat visual offsets for 0.3.DL-R1 are intentionally not guessed.
            break;
        default:
            break;
    }

    if (!addrOpenChat)
    {
        LOG_FATAL("[ChatHook] Unsupported SA:MP version for OpenChatInput.");
        return false;
    }

    if (!hooks_.Install("ChatHook::OpenChatInput", addrOpenChat, reinterpret_cast<void*>(&Hook_OpenChatInput)))
    {
        LOG_ERROR("[ChatHook] Failed to install OpenChatInput hook.");
        return false;
    }

    if (!hooks_.Install("ChatHook::CloseChatInput", addrCloseChat, reinterpret_cast<void*>(&Hook_CloseChatInput)))
    {
        LOG_ERROR("[ChatHook] Failed to install CloseChatInput hook.");
        hooks_.Uninstall("ChatHook::OpenChatInput");
        return false;
    }

    s_orig_open_ = reinterpret_cast<FnOpenChatInput>(hooks_.GetOriginal("ChatHook::OpenChatInput"));
    if (!s_orig_open_)
    {
        LOG_FATAL("[ChatHook] Failed to get original OpenChatInput function pointer.");
        hooks_.Uninstall("ChatHook::CloseChatInput");
        hooks_.Uninstall("ChatHook::OpenChatInput");
        return false;
    }

    s_orig_close_ = reinterpret_cast<FnCloseChatInput>(hooks_.GetOriginal("ChatHook::CloseChatInput"));
    if (!s_orig_close_)
    {
        LOG_FATAL("[ChatHook] Failed to get original CloseChatInput function pointer.");
        hooks_.Uninstall("ChatHook::CloseChatInput");
        hooks_.Uninstall("ChatHook::OpenChatInput");
        s_orig_open_ = nullptr;
        return false;
    }

    if (addrRenderChat && hooks_.Install("ChatHook::RenderChat", addrRenderChat, reinterpret_cast<void*>(&Hook_RenderChat)))
        s_orig_render_ = reinterpret_cast<FnChatVisual>(hooks_.GetOriginal("ChatHook::RenderChat"));
    else if (addrRenderChat)
        LOG_ERROR("[ChatHook] Failed to install native chat Render hook.");

    if (addrDrawChat && hooks_.Install("ChatHook::DrawChat", addrDrawChat, reinterpret_cast<void*>(&Hook_DrawChat)))
        s_orig_draw_ = reinterpret_cast<FnChatVisual>(hooks_.GetOriginal("ChatHook::DrawChat"));
    else if (addrDrawChat)
        LOG_ERROR("[ChatHook] Failed to install native chat Draw hook.");

    if (addrRenderChatToSurface && hooks_.Install("ChatHook::RenderChatToSurface", addrRenderChatToSurface, reinterpret_cast<void*>(&Hook_RenderChatToSurface)))
        s_orig_render_surface_ = reinterpret_cast<FnChatVisual>(hooks_.GetOriginal("ChatHook::RenderChatToSurface"));
    else if (addrRenderChatToSurface)
        LOG_ERROR("[ChatHook] Failed to install native chat RenderToSurface hook.");

    if (addrRenderChat || addrDrawChat || addrRenderChatToSurface)
        LOG_INFO("[ChatHook] Native SA:MP chat visuals suppressed and scrollbar control forced hidden.");
    else
        LOG_WARN("[ChatHook] Native chat visual suppression is unavailable for this SA:MP version.");

    LOG_INFO("[ChatHook] Native T chat is blocked unless an overlay CEF browser has controls_chat=true.");
    LOG_DEBUG("[ChatHook] OpenChatInput hook installed.");
    LOG_DEBUG("[ChatHook] CloseChatInput hook installed.");
    return true;
}

void ChatHook::Shutdown()
{
    hooks_.Uninstall("ChatHook::RenderChatToSurface");
    hooks_.Uninstall("ChatHook::DrawChat");
    hooks_.Uninstall("ChatHook::RenderChat");
    hooks_.Uninstall("ChatHook::OpenChatInput");
    hooks_.Uninstall("ChatHook::CloseChatInput");
    s_orig_render_surface_ = nullptr;
    s_orig_draw_ = nullptr;
    s_orig_render_ = nullptr;
    s_orig_open_ = nullptr;
    s_orig_close_ = nullptr;
    s_self_ = nullptr;
}

void ChatHook::SetChatInputState(bool open)
{
    if (focus_.IsChatInputOpen() == open)
        return;

    focus_.SetChatInputOpen(open);

    for (const auto& kv : browser_.GetAllBrowsers())
    {
        const int id = kv.first;
        auto* inst = browser_.GetBrowserInstance(id);
        if (!inst || !inst->browser)
            continue;

        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("emit_event");
        CefRefPtr<CefListValue> list = msg->GetArgumentList();
        list->SetString(0, EnsureUtf8ForCef("omp:cef:internal:chatInputState"));
        list->SetBool(1, open);
        inst->browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
    }

    ClientEmitEventPacket ev;
    ev.browserId = -1;
    ev.name = CefEvent::Client::ChatInputState;
    ev.args.emplace_back(open);
    network_.SendPacket(PacketType::ClientEmitEvent, ev);
}

void __fastcall ChatHook::Hook_OpenChatInput(void* pThis, void* _edx)
{
    auto* self = s_self_;
    if (!self)
        return;

    if (self->focus_.ShouldBlockChat())
        return;

    // Do not let SA:MP/open.mp open its native T chat on servers that did not
    // create a CEF overlay explicitly allowed to control chat input.
    if (!HasCefChatInputController(self->browser_))
    {
        self->SetChatInputState(false);
        HideNativeChatScrollbar(pThis);
        return;
    }

    // Keep the native input state only as an invisible keyboard lifecycle bridge
    // for CEF chat (typing, Enter and Escape). Native rendering remains suppressed.
    if (s_orig_open_)
        s_orig_open_(pThis, _edx);

    HideNativeChatScrollbar(pThis);
    self->SetChatInputState(true);
}

void __fastcall ChatHook::Hook_CloseChatInput(void* pThis, void* _edx)
{
    auto* self = s_self_;
    if (!self)
        return;

    if (s_orig_close_)
        s_orig_close_(pThis, _edx);

    HideNativeChatScrollbar(pThis);
    self->SetChatInputState(false);
}

void __fastcall ChatHook::Hook_RenderChat(void* pThis, void* /*_edx*/)
{
    HideNativeChatScrollbar(pThis);
    // Native CChat drawing is intentionally suppressed.
}

void __fastcall ChatHook::Hook_DrawChat(void* pThis, void* /*_edx*/)
{
    HideNativeChatScrollbar(pThis);
    // Native chat text/background drawing is intentionally suppressed.
}

void __fastcall ChatHook::Hook_RenderChatToSurface(void* pThis, void* /*_edx*/)
{
    HideNativeChatScrollbar(pThis);
    // Native chat surface rendering is intentionally suppressed.
}
