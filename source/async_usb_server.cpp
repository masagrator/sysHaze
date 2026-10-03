/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <haze.hpp>

namespace haze {

    namespace {

        constinit UsbSession g_usb_session;

    }

    Result AsyncUsbServer::Initialize(const UsbCommsInterfaceInfo *interface_info, u16 id_vendor, u16 id_product, EventReactor *reactor) {
        m_reactor = reactor;

        /* Set up a new USB session. */
        R_TRY(g_usb_session.Initialize(interface_info, id_vendor, id_product));

        R_SUCCEED();
    }

    void AsyncUsbServer::Finalize() {
        g_usb_session.Finalize();
    }

    Result AsyncUsbServer::TransferPacketImpl(bool read, void *page, u32 size, u32 *out_size_transferred) const {
        u32 urb_id;
        s32 waiter_idx;

        /* If we're not configured yet, wait to become configured first. */
        if (!g_usb_session.GetConfigured()) {
            R_TRY(m_reactor->WaitFor(std::addressof(waiter_idx), waiterForEvent(usbDsGetStateChangeEvent())));
            R_TRY(eventClear(usbDsGetStateChangeEvent()));

            R_THROW(haze::ResultNotConfigured());
        }

        /* Select the appropriate endpoint and begin a transfer. */
        UsbSessionEndpoint ep = read ? UsbSessionEndpoint_Read : UsbSessionEndpoint_Write;
        R_TRY(g_usb_session.TransferAsync(ep, page, size, std::addressof(urb_id)));

        /* Try to wait for the event. */
        R_TRY(m_reactor->WaitFor(std::addressof(waiter_idx), waiterForEvent(g_usb_session.GetCompletionEvent(ep))));

        /* Return what we transferred. */
        R_RETURN(g_usb_session.GetTransferResult(ep, urb_id, out_size_transferred));
    }

    Result AsyncUsbServer::ReadPacketWhileIdle(void *page, u32 size, u32 *out_size_transferred, UsbIdleHandler *idle_handler) const {
        u32 urb_id;
        s32 waiter_idx;

        /* If we're not configured yet, wait to become configured first. */
        if (!g_usb_session.GetConfigured()) {
            R_TRY(m_reactor->WaitFor(std::addressof(waiter_idx), waiterForEvent(usbDsGetStateChangeEvent())));
            R_TRY(eventClear(usbDsGetStateChangeEvent()));

            R_THROW(haze::ResultNotConfigured());
        }

        /* Begin the read transfer. */
        R_TRY(g_usb_session.TransferAsync(UsbSessionEndpoint_Read, page, size, std::addressof(urb_id)));

        while (true) {
            /* Wait for the read, and for any outstanding event transfer. */
            Waiter waiters[2] = { waiterForEvent(g_usb_session.GetCompletionEvent(UsbSessionEndpoint_Read)) };
            s32 num_waiters = 1;
            if (idle_handler->IsInterruptTransferPending()) {
                waiters[num_waiters++] = waiterForEvent(g_usb_session.GetCompletionEvent(UsbSessionEndpoint_Interrupt));
            }

            const Result rc = m_reactor->WaitForWithTimeout(std::addressof(waiter_idx), waiters, num_waiters, idle_handler->GetIdleTimeoutNs());
            if (svc::ResultTimedOut::Includes(rc)) {
                /* The host is idle; give the handler a chance to do background work. */
                idle_handler->OnIdleTimeout();
                continue;
            }
            R_TRY(rc);

            if (waiter_idx == 0) {
                /* The host sent us something. */
                break;
            }

            /* An event transfer completed. */
            idle_handler->OnInterruptTransferComplete();
        }

        /* Return what we transferred. */
        R_RETURN(g_usb_session.GetTransferResult(UsbSessionEndpoint_Read, urb_id, out_size_transferred));
    }

    Result AsyncUsbServer::PostInterruptAsync(void *page, u32 size, u32 *out_urb_id) const {
        R_RETURN(g_usb_session.TransferAsync(UsbSessionEndpoint_Interrupt, page, size, out_urb_id));
    }

    Result AsyncUsbServer::GetInterruptResult(u32 urb_id, u32 *out_size_transferred) const {
        R_RETURN(g_usb_session.GetTransferResult(UsbSessionEndpoint_Interrupt, urb_id, out_size_transferred));
    }

    Event *AsyncUsbServer::GetInterruptCompletionEvent() const {
        return g_usb_session.GetCompletionEvent(UsbSessionEndpoint_Interrupt);
    }

    bool AsyncUsbServer::IsConfigured() const {
        return g_usb_session.GetConfigured();
    }

}
