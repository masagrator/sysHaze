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
#include <haze/ptp_responder_types.hpp>

/*
 * Change notification.
 *
 * MTP hosts (Windows WPD in particular) cache every directory listing they
 * fetch and never ask again on their own; F5 in Explorer does not re-issue
 * GetObjectHandles.  The device is expected to push ObjectAdded /
 * ObjectRemoved / ObjectInfoChanged events on the interrupt endpoint when
 * its storage changes.  Android does this from inotify on every directory
 * the host has enumerated.
 *
 * Horizon has no inotify, so while the host is idle we periodically re-read
 * every directory the host has enumerated ("visited" directories), diff the
 * listing against the object database, update the database, and queue the
 * matching events.  Changes made by the host itself go through the database
 * directly and therefore never generate events.
 */

namespace haze {

    void PtpResponder::ClearPendingEvents() {
        m_pending_event_head  = 0;
        m_pending_event_count = 0;
    }

    bool PtpResponder::QueueEvent(PtpEventCode code, u32 handle) {
        if (m_pending_event_count == MaxPendingEvents) {
            return false;
        }

        const size_t idx = (m_pending_event_head + m_pending_event_count) % MaxPendingEvents;
        m_pending_events[idx] = { static_cast<u16>(code), handle };
        m_pending_event_count++;

        this->SendNextEvent();
        return true;
    }

    void PtpResponder::SendNextEvent() {
        if (m_event_in_flight || m_pending_event_count == 0 || !m_session_open) {
            return;
        }

        const PendingEvent &ev = m_pending_events[m_pending_event_head];

        /* Build the event container: length, type, code, transaction id, one parameter. */
        u8 * const buf = m_buffers->usb_interrupt_buffer;
        const u32 length   = PtpUsbBulkHeaderLength + sizeof(u32);
        const u16 type     = PtpUsbBulkContainerType_Event;
        const u16 code     = ev.code;
        const u32 trans_id = m_request_header.trans_id;
        const u32 param    = ev.handle;
        std::memcpy(buf + 0x0, std::addressof(length),   sizeof(length));
        std::memcpy(buf + 0x4, std::addressof(type),     sizeof(type));
        std::memcpy(buf + 0x6, std::addressof(code),     sizeof(code));
        std::memcpy(buf + 0x8, std::addressof(trans_id), sizeof(trans_id));
        std::memcpy(buf + 0xC, std::addressof(param),    sizeof(param));

        if (R_SUCCEEDED(m_usb_server.PostInterruptAsync(buf, length, std::addressof(m_event_urb_id)))) {
            m_event_in_flight = true;
            m_pending_event_head = (m_pending_event_head + 1) % MaxPendingEvents;
            m_pending_event_count--;
        }
    }

    void PtpResponder::OnInterruptTransferComplete() {
        u32 transferred;
        m_usb_server.GetInterruptResult(m_event_urb_id, std::addressof(transferred));
        m_event_in_flight = false;

        this->SendNextEvent();
    }

    s64 PtpResponder::GetIdleTimeoutNs() {
        /* Nothing to watch without a session. */
        if (!m_session_open) {
            return -1;
        }

        const u64 now = armGetSystemTick();
        if (now >= m_next_poll_tick) {
            return 0;
        }
        return static_cast<s64>(armTicksToNs(m_next_poll_tick - now));
    }

    void PtpResponder::OnIdleTimeout() {
        m_next_poll_tick = armGetSystemTick() + armNsToTicks(PollIntervalNs);

        /* Retry delivery in case a previous post failed. */
        this->SendNextEvent();

        this->PollForChanges();
    }

    void PtpResponder::PollForChanges() {
        if (!m_session_open || m_pending_event_count == MaxPendingEvents) {
            return;
        }

        /* Pick up to MaxDirectoriesPerPoll watched directories, round-robin by object ID. */
        u32 dirs[MaxDirectoriesPerPoll];
        size_t num_dirs = 0;

        m_object_database.ForEachObject([&](PtpObject &o) {
            if (o.m_visited && o.GetObjectId() > m_poll_cursor && num_dirs < MaxDirectoriesPerPoll) {
                dirs[num_dirs++] = o.GetObjectId();
            }
        });
        m_object_database.ForEachObject([&](PtpObject &o) {
            if (o.m_visited && o.GetObjectId() <= m_poll_cursor && num_dirs < MaxDirectoriesPerPoll) {
                dirs[num_dirs++] = o.GetObjectId();
            }
        });

        m_poll_cursor = (num_dirs == MaxDirectoriesPerPoll) ? dirs[num_dirs - 1] : 0;

        for (size_t i = 0; i < num_dirs && m_pending_event_count < MaxPendingEvents; i++) {
            this->PollDirectory(dirs[i]);
        }
    }

    void PtpResponder::PollDirectory(u32 dir_id) {
        auto * const dir_obj = m_object_database.GetObjectById(dir_id);
        if (dir_obj == nullptr || !dir_obj->m_visited) {
            return;
        }

        /* If the directory itself is gone, its parent's poll will report it. */
        FsDir dir;
        if (R_FAILED(m_fs.OpenDirectory(dir_obj->GetName(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, std::addressof(dir)))) {
            return;
        }

        bool listing_complete = true;
        {
            ON_SCOPE_EXIT { m_fs.CloseDirectory(std::addressof(dir)); };

            /* Clear marks on everything we currently believe is in this directory. */
            m_object_database.ForEachObject([&](PtpObject &o) {
                if (o.GetParentId() == dir_id) {
                    o.m_seen = false;
                }
            });

            char * const path = m_buffers->poll_path_buffer;
            const size_t parent_len = std::strlen(dir_obj->GetName());

            while (true) {
                s64 read_count = 0;
                if (R_FAILED(m_fs.ReadDirectory(std::addressof(dir), std::addressof(read_count), DirectoryReadSize, m_buffers->file_system_entry_buffer))) {
                    listing_complete = false;
                    break;
                }

                for (s64 i = 0; i < read_count; i++) {
                    const FsDirectoryEntry &entry = m_buffers->file_system_entry_buffer[i];
                    const bool is_dir = entry.type == FsDirEntryType_Dir;
                    const s64 size    = is_dir ? 0 : entry.file_size;

                    /* Build the full object name exactly as GetObjectHandles would. */
                    const size_t name_len = strnlen(entry.name, sizeof(entry.name));
                    if (parent_len + 1 + name_len + 1 > sizeof(m_buffers->poll_path_buffer)) {
                        continue;
                    }
                    std::memcpy(path, dir_obj->GetName(), parent_len);
                    path[parent_len] = '/';
                    std::memcpy(path + parent_len + 1, entry.name, name_len);
                    path[parent_len + 1 + name_len] = '\0';

                    if (auto * const child = m_object_database.GetObjectByName(path); child != nullptr) {
                        /* Known object: check whether it changed. */
                        child->m_seen = true;
                        if (child->GetParentId() == dir_id && (child->m_is_dir != is_dir || child->m_size != size)) {
                            if (this->QueueEvent(PtpEventCode_ObjectInfoChanged, child->GetObjectId())) {
                                child->m_is_dir = is_dir;
                                child->m_size   = size;
                            }
                        }
                        continue;
                    }

                    /* New object. If we cannot announce it right now, leave it for the next poll. */
                    if (m_pending_event_count == MaxPendingEvents) {
                        listing_complete = false;
                        continue;
                    }

                    PtpObject *child;
                    if (R_FAILED(m_object_database.CreateOrFindObject(dir_obj->GetName(), entry.name, dir_id, std::addressof(child)))) {
                        listing_complete = false;
                        continue;
                    }
                    m_object_database.RegisterObject(child);
                    child->m_is_dir = is_dir;
                    child->m_size   = size;
                    child->m_seen   = true;

                    this->QueueEvent(PtpEventCode_ObjectAdded, child->GetObjectId());
                }

                if (read_count < DirectoryReadSize) {
                    break;
                }
            }
        }

        /* Only remove objects if we are sure we saw the whole directory. */
        if (!listing_complete) {
            return;
        }

        /* Anything not seen was removed behind our back. */
        while (m_pending_event_count < MaxPendingEvents) {
            PtpObject *removed[SweepBatchSize];
            size_t num_removed = 0;
            const size_t limit = std::min(SweepBatchSize, MaxPendingEvents - m_pending_event_count);

            m_object_database.ForEachObject([&](PtpObject &o) {
                if (num_removed < limit && o.GetParentId() == dir_id && !o.m_seen && !this->IsStorageRoot(o.GetObjectId())) {
                    removed[num_removed++] = std::addressof(o);
                }
            });

            if (num_removed == 0) {
                break;
            }

            for (size_t i = 0; i < num_removed; i++) {
                this->QueueEvent(PtpEventCode_ObjectRemoved, removed[i]->GetObjectId());
                m_object_database.DeleteObjectTree(removed[i]);
            }
        }
    }

    void PtpResponder::UpdateObjectSize(PtpObject *obj, FsFile *file) {
        s64 size;
        if (R_SUCCEEDED(m_fs.GetFileSize(file, std::addressof(size)))) {
            obj->m_size = size;
        }
    }

}
