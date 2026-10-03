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
#pragma once

#include <haze/common.hpp>
#include <haze/async_usb_server.hpp>
#include <haze/ptp_object_heap.hpp>
#include <haze/ptp_object_database.hpp>
#include <haze/ptp_responder_types.hpp>

namespace haze {

    class PtpDataParser;

    class PtpResponder final : public UsbIdleHandler {
        private:
            /* Change notification (MTP events) state. */
            struct PendingEvent {
                u16 code;
                u32 handle;
            };
            static constexpr size_t MaxPendingEvents      = 64;
            static constexpr s64    PollIntervalNs        = 1'000'000'000;
            static constexpr size_t MaxDirectoriesPerPoll = 32;
            static constexpr size_t SweepBatchSize        = 32;

            PendingEvent m_pending_events[MaxPendingEvents];
            size_t m_pending_event_head;
            size_t m_pending_event_count;
            u32 m_event_urb_id;
            bool m_event_in_flight;
            u64 m_next_poll_tick;
            u32 m_poll_cursor;
        private:
            AsyncUsbServer m_usb_server;
            FileSystemProxy m_fs;
            PtpUsbBulkContainer m_request_header;
            PtpObjectHeap *m_object_heap;
            PtpBuffers* m_buffers;
            u32 m_send_object_id;
            bool m_session_open;

            PtpObjectDatabase m_object_database;
        private:
            /* Custom partition support (loaded from INI config at Initialize). */
            struct CustomPartition {
                u32  storage_id;
                char name[64];         /* Display name (INI section header). */
                char root_path[0x301]; /* Absolute path on sdmc (value of Path= key). */
            };
            static constexpr size_t MaxCustomPartitions = 8;
            CustomPartition m_custom_partitions[MaxCustomPartitions];
            size_t m_custom_partition_count;
        public:
            constexpr explicit PtpResponder() : m_pending_events(), m_pending_event_head(), m_pending_event_count(), m_event_urb_id(), m_event_in_flight(), m_next_poll_tick(), m_poll_cursor(), m_usb_server(), m_fs(), m_request_header(), m_object_heap(), m_buffers(), m_send_object_id(), m_session_open(), m_object_database(), m_custom_partitions(), m_custom_partition_count(0) { /* ... */ }

            Result Initialize(EventReactor *reactor, PtpObjectHeap *object_heap);
            void Finalize();
        public:
            Result LoopProcess();
        private:
            /* Custom partition helpers. */
            void LoadCustomPartitions();
            bool IsStorageRoot(u32 object_id) const;
            const CustomPartition *FindCustomPartitionById(u32 storage_id) const;
            u32 GetStorageForObject(u32 object_id);
            u32 GetReportedParentId(const PtpObject *obj) const;
        public:
            /* UsbIdleHandler. */
            s64 GetIdleTimeoutNs() override;
            void OnIdleTimeout() override;
            bool IsInterruptTransferPending() override { return m_event_in_flight; }
            void OnInterruptTransferComplete() override;
        private:
            /* Change detection and event delivery. */
            void ClearPendingEvents();
            bool QueueEvent(PtpEventCode code, u32 handle);
            void SendNextEvent();
            void PollForChanges();
            void PollDirectory(u32 dir_id);
            void UpdateObjectSize(PtpObject *obj, FsFile *file);
        private:
            /* Request handling. */
            Result HandleRequest();
            Result HandleRequestImpl();
            Result HandleCommandRequest(PtpDataParser &dp);
            void ForceCloseSession();

            Result WriteResponse(PtpResponseCode code, const void* data, size_t size);
            Result WriteResponse(PtpResponseCode code);

            template <typename Data> requires (util::is_pod<Data>::value)
            Result WriteResponse(PtpResponseCode code, const Data &data) {
                R_RETURN(this->WriteResponse(code, std::addressof(data), sizeof(data)));
            }

            /* PTP operations. */
            Result GetDeviceInfo(PtpDataParser &dp);
            Result OpenSession(PtpDataParser &dp);
            Result CloseSession(PtpDataParser &dp);
            Result GetStorageIds(PtpDataParser &dp);
            Result GetStorageInfo(PtpDataParser &dp);
            Result GetObjectHandles(PtpDataParser &dp);
            Result GetObjectInfo(PtpDataParser &dp);
            Result GetObject(PtpDataParser &dp);
            Result SendObjectInfo(PtpDataParser &dp);
            Result SendObject(PtpDataParser &dp);
            Result DeleteObject(PtpDataParser &dp);

            /* Android operations. */
            Result GetPartialObject64(PtpDataParser &dp);
            Result SendPartialObject(PtpDataParser &dp);
            Result TruncateObject(PtpDataParser &dp);
            Result BeginEditObject(PtpDataParser &dp);
            Result EndEditObject(PtpDataParser &dp);

            /* MTP operations. */
            Result GetObjectPropsSupported(PtpDataParser &dp);
            Result GetObjectPropDesc(PtpDataParser &dp);
            Result GetObjectPropValue(PtpDataParser &dp);
            Result SetObjectPropValue(PtpDataParser &dp);
            Result GetObjectPropList(PtpDataParser &dp);
    };

}
