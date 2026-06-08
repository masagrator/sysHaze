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
#include <haze/ptp_data_builder.hpp>
#include <haze/ptp_data_parser.hpp>
#include <haze/ptp_responder_types.hpp>

namespace haze {

    Result PtpResponder::GetDeviceInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Write the device info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(MtpStandardVersion));
            R_TRY(db.Add(MtpVendorExtensionId));
            R_TRY(db.Add(MtpStandardVersion));
            R_TRY(db.AddString(MtpVendorExtensionDesc));
            R_TRY(db.Add(MtpFunctionalModeDefault));
            R_TRY(db.AddArray(SupportedOperationCodes, util::size(SupportedOperationCodes)));
            R_TRY(db.AddArray(SupportedEventCodes, util::size(SupportedEventCodes)));
            R_TRY(db.AddArray(SupportedDeviceProperties, util::size(SupportedDeviceProperties)));
            R_TRY(db.AddArray(SupportedCaptureFormats, util::size(SupportedCaptureFormats)));
            R_TRY(db.AddArray(SupportedPlaybackFormats, util::size(SupportedPlaybackFormats)));
            R_TRY(db.AddString(MtpDeviceManufacturer));
            R_TRY(db.AddString(MtpDeviceModel));
            R_TRY(db.AddString(GetFirmwareVersion()));
            R_TRY(db.AddString(GetSerialNumber()));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::OpenSession(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        /* Close, if we're already open. */
        this->ForceCloseSession();

        /* Re-read the INI config on every session open so that changes made to
         * sdmc:/config/sysHaze/config.ini take effect the next time Windows
         * reconnects, without needing to restart the sysmodule. */
        this->LoadCustomPartitions();

        /* Initialize the database. */
        m_session_open = true;
        m_object_database.Initialize(m_object_heap);

        /* Register the SD card storage root. */
        PtpObject *object;
        R_TRY(m_object_database.CreateOrFindObject("", "", PtpGetObjectHandles_RootParent, std::addressof(object)));
        m_object_database.RegisterObject(object, StorageId_SdmcFs);

        /* Register each custom partition root.
         *
         * The root path (e.g. "/SaltySD/FPSLocker") is used as the `name`
         * argument so that the resulting object path becomes "//SaltySD/FPSLocker".
         * The Switch FS normalises leading double-slashes to a single slash, so
         * all subsequent file operations resolve correctly.  Custom partition
         * storage IDs (StorageId_Custom0 = 0x00020001 upward) are numerically
         * above StorageId_SdmcFs (0x00010001) and far above the auto-increment
         * object handle counter (starts at 1), so there is no collision risk. */
        for (size_t i = 0; i < m_custom_partition_count; i++) {
            const auto &part = m_custom_partitions[i];
            PtpObject *root;
            R_TRY(m_object_database.CreateOrFindObject("", part.root_path, PtpGetObjectHandles_RootParent, std::addressof(root)));
            m_object_database.RegisterObject(root, part.storage_id);
        }

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::CloseSession(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        this->ForceCloseSession();

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetStorageIds(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Build the storage ID array: SD card first, then any custom partitions. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            const u32 total = 1 + static_cast<u32>(m_custom_partition_count);
            R_TRY(db.Add(total));

            R_TRY(db.Add(static_cast<u32>(StorageId_SdmcFs)));
            for (size_t i = 0; i < m_custom_partition_count; i++) {
                R_TRY(db.Add(m_custom_partitions[i].storage_id));
            }
            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetStorageInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));
        PtpStorageInfo storage_info(DefaultStorageInfo);

        /* Get the storage ID the client requested information for. */
        u32 storage_id;
        R_TRY(dp.Read(std::addressof(storage_id)));
        R_TRY(dp.Finalize());

        /* Get space info from fs.  Size figures always reflect the whole SD card;
         * per-directory usage is too expensive to compute in a sysmodule.
         * Custom partitions additionally verify the path exists: a missing folder
         * reports 0/0 to signal unavailability without breaking the session. */
        if (storage_id == StorageId_SdmcFs) {
            s64 total_space, free_space;
            R_TRY(m_fs.GetTotalSpace("/", std::addressof(total_space)));
            R_TRY(m_fs.GetFreeSpace("/", std::addressof(free_space)));

            storage_info.max_capacity        = total_space;
            storage_info.free_space_in_bytes = free_space;
            storage_info.storage_description = "!SD Card";
        } else if (const auto *part = this->FindCustomPartitionById(storage_id); part != nullptr) {
            /* Check existence by attempting to open the directory, using the same
             * path the object database stores (obj->GetName(), which has the
             * leading // produced by CreateOrFindObject).  GetEntryType is not
             * used here because it is stricter about path normalisation than
             * OpenDirectory and would fail even for valid paths in this codebase. */
            s64 total_space = 0, free_space = 0;

            auto * const root_obj = m_object_database.GetObjectById(part->storage_id);
            if (root_obj != nullptr) {
                FsDir check_dir;
                if (R_SUCCEEDED(m_fs.OpenDirectory(root_obj->GetName(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, std::addressof(check_dir)))) {
                    m_fs.CloseDirectory(std::addressof(check_dir));
                    R_TRY(m_fs.GetTotalSpace("/", std::addressof(total_space)));
                    R_TRY(m_fs.GetFreeSpace("/", std::addressof(free_space)));
                }
            }

            storage_info.max_capacity        = total_space;
            storage_info.free_space_in_bytes = free_space;
            storage_info.storage_description = part->name;
        } else {
            R_THROW(haze::ResultInvalidStorageId());
        }

        /* Write the storage info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(storage_info.storage_type));
            R_TRY(db.Add(storage_info.filesystem_type));
            R_TRY(db.Add(storage_info.access_capability));
            R_TRY(db.Add(storage_info.max_capacity));
            R_TRY(db.Add(storage_info.free_space_in_bytes));
            R_TRY(db.Add(storage_info.free_space_in_images));
            R_TRY(db.AddString(storage_info.storage_description));
            R_TRY(db.AddString(storage_info.volume_label));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectHandles(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested enumeration for. */
        u32 storage_id, object_format_code, association_object_handle;
        R_TRY(dp.Read(std::addressof(storage_id)));
        R_TRY(dp.Read(std::addressof(object_format_code)));
        R_TRY(dp.Read(std::addressof(association_object_handle)));
        R_TRY(dp.Finalize());

        /* Handle top-level requests. */
        if (storage_id == PtpGetObjectHandles_AllStorage) {
            storage_id = StorageId_SdmcFs;
        }

        /* Rewrite requests for enumerating storage directories. */
        if (association_object_handle == PtpGetObjectHandles_RootParent) {
            association_object_handle = storage_id;
        }

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(association_object_handle);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Try to read the object as a directory.  For custom partition roots whose
         * configured folder is absent, respond with AccessDenied so Windows shows
         * a visible error dialog.  InvalidStorageId is silently swallowed by
         * Explorer (it just shows an empty folder), so we write the response
         * directly here rather than throwing through the R_CATCH dispatch. */
        FsDir dir;
        const Result open_rc = m_fs.OpenDirectory(obj->GetName(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, std::addressof(dir));
        if (R_FAILED(open_rc)) {
            if (this->FindCustomPartitionById(association_object_handle) != nullptr) {
                R_RETURN(this->WriteResponse(PtpResponseCode_AccessDenied));
            }
            R_RETURN(open_rc);
        }

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { m_fs.CloseDirectory(std::addressof(dir)); };

        /* Count how many entries are in the directory. */
        s64 entry_count = 0;
        R_TRY(m_fs.GetDirectoryEntryCount(std::addressof(dir), std::addressof(entry_count)));

        /* Begin writing. */
        R_TRY(db.AddDataHeader(m_request_header, sizeof(u32) + (entry_count * sizeof(u32))));
        R_TRY(db.Add(static_cast<u32>(entry_count)));

        /* Enumerate the directory, writing results to the data builder as we progress. */
        /* TODO: How should we handle the directory contents changing during enumeration? */
        /* Is this even feasible to handle? */
        while (true) {
            /* Get the next batch. */
            s64 read_count = 0;
            R_TRY(m_fs.ReadDirectory(std::addressof(dir), std::addressof(read_count), DirectoryReadSize, m_buffers->file_system_entry_buffer));

            /* Write to output. */
            for (s64 i = 0; i < read_count; i++) {
                const char *name = m_buffers->file_system_entry_buffer[i].name;
                u32 handle;

                R_TRY(m_object_database.CreateAndRegisterObjectId(obj->GetName(), name, obj->GetObjectId(), std::addressof(handle)));
                R_TRY(db.Add(handle));
            }

            /* If we read fewer than the batch size, we're done. */
            if (read_count < DirectoryReadSize) {
                break;
            }
        }

        /* Flush the data response. */
        R_TRY(db.Commit());

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested info for. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Build info about the object. */
        PtpObjectInfo object_info(DefaultObjectInfo);

        if (object_id == StorageId_SdmcFs) {
            /* The SD Card root has some special properties. */
            object_info.storage_id    = StorageId_SdmcFs;
            object_info.object_format    = PtpObjectFormatCode_Association;
            object_info.association_type = PtpAssociationType_GenericFolder;
            object_info.filename         = "!SD Card";
        } else if (const auto *part = this->FindCustomPartitionById(object_id); part != nullptr) {
            /* Custom partition root: show the configured display name. */
            object_info.storage_id       = static_cast<StorageId>(part->storage_id);
            object_info.object_format    = PtpObjectFormatCode_Association;
            object_info.association_type = PtpAssociationType_GenericFolder;
            object_info.filename         = part->name;
        } else {
            /* Regular file or directory. Determine which storage it belongs to. */
            object_info.storage_id = static_cast<StorageId>(this->GetStorageForObject(object_id));

            /* Figure out what type of object this is. */
            FsDirEntryType entry_type;
            R_TRY(m_fs.GetEntryType(obj->GetName(), std::addressof(entry_type)));

            /* Get the size, if we are requesting info about a file. */
            s64 size = 0;
            if (entry_type == FsDirEntryType_File) {
                FsFile file;
                R_TRY(m_fs.OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

                /* Ensure we maintain a clean state on exit. */
                ON_SCOPE_EXIT { m_fs.CloseFile(std::addressof(file)); };

                R_TRY(m_fs.GetFileSize(std::addressof(file), std::addressof(size)));
            }

            object_info.filename               = std::strrchr(obj->GetName(), '/') + 1;
            object_info.object_compressed_size = size;
            object_info.parent_object          = obj->GetParentId();

            if (entry_type == FsDirEntryType_Dir) {
                object_info.object_format    = PtpObjectFormatCode_Association;
                object_info.association_type = PtpAssociationType_GenericFolder;
            } else {
                object_info.object_format    = PtpObjectFormatCode_Undefined;
                object_info.association_type = PtpAssociationType_Undefined;
            }
        }

        /* Write the object info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(object_info.storage_id));
            R_TRY(db.Add(object_info.object_format));
            R_TRY(db.Add(object_info.protection_status));
            R_TRY(db.Add(object_info.object_compressed_size));
            R_TRY(db.Add(object_info.thumb_format));
            R_TRY(db.Add(object_info.thumb_compressed_size));
            R_TRY(db.Add(object_info.thumb_width));
            R_TRY(db.Add(object_info.thumb_height));
            R_TRY(db.Add(object_info.image_width));
            R_TRY(db.Add(object_info.image_height));
            R_TRY(db.Add(object_info.image_depth));
            R_TRY(db.Add(object_info.parent_object));
            R_TRY(db.Add(object_info.association_type));
            R_TRY(db.Add(object_info.association_desc));
            R_TRY(db.Add(object_info.sequence_number));
            R_TRY(db.AddString(object_info.filename));
            R_TRY(db.AddString(object_info.capture_date));
            R_TRY(db.AddString(object_info.modification_date));
            R_TRY(db.AddString(object_info.keywords));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObject(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Lock the object as a file. */
        FsFile file;
        R_TRY(m_fs.OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { m_fs.CloseFile(std::addressof(file)); };

        /* Get the file's size. */
        s64 size = 0;
        R_TRY(m_fs.GetFileSize(std::addressof(file), std::addressof(size)));

        /* Send the header and file size. */
        R_TRY(db.AddDataHeader(m_request_header, size));

        /* Begin reading the file, writing data to the builder as we progress. */
        s64 offset = 0;
        while (true) {
            /* Get the next batch. */
            u64 bytes_read;
            R_TRY(m_fs.ReadFile(std::addressof(file), offset, m_buffers->file_system_data_buffer, FsBufferSize, FsReadOption_None, std::addressof(bytes_read)));

            offset += bytes_read;

            /* Write to output. */
            R_TRY(db.AddBuffer(m_buffers->file_system_data_buffer, bytes_read));

            /* If we read fewer bytes than the batch size, we're done. */
            if (bytes_read < FsBufferSize) {
                break;
            }
        }

        /* Flush the data response. */
        R_TRY(db.Commit());

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::SendObjectInfo(PtpDataParser &rdp) {
        /* Get the storage ID and parent object and flush the request packet. */
        u32 storage_id, parent_object;
        R_TRY(rdp.Read(std::addressof(storage_id)));
        R_TRY(rdp.Read(std::addressof(parent_object)));
        R_TRY(rdp.Finalize());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));
        PtpObjectInfo info(DefaultObjectInfo);

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,  haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,         haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id, haze::ResultOperationNotSupported());

        /* Read in the object info. */
        R_TRY(dp.Read(std::addressof(info.storage_id)));
        R_TRY(dp.Read(std::addressof(info.object_format)));
        R_TRY(dp.Read(std::addressof(info.protection_status)));
        R_TRY(dp.Read(std::addressof(info.object_compressed_size)));
        R_TRY(dp.Read(std::addressof(info.thumb_format)));
        R_TRY(dp.Read(std::addressof(info.thumb_compressed_size)));
        R_TRY(dp.Read(std::addressof(info.thumb_width)));
        R_TRY(dp.Read(std::addressof(info.thumb_height)));
        R_TRY(dp.Read(std::addressof(info.image_width)));
        R_TRY(dp.Read(std::addressof(info.image_height)));
        R_TRY(dp.Read(std::addressof(info.image_depth)));
        R_TRY(dp.Read(std::addressof(info.parent_object)));
        R_TRY(dp.Read(std::addressof(info.association_type)));
        R_TRY(dp.Read(std::addressof(info.association_desc)));
        R_TRY(dp.Read(std::addressof(info.sequence_number)));
        R_TRY(dp.ReadString(m_buffers->filename_string_buffer));
        R_TRY(dp.ReadString(m_buffers->capture_date_string_buffer));
        R_TRY(dp.ReadString(m_buffers->modification_date_string_buffer));
        R_TRY(dp.ReadString(m_buffers->keywords_string_buffer));
        R_TRY(dp.Finalize());

        /* Rewrite requests for creating in storage directories. */
        if (parent_object == PtpGetObjectHandles_RootParent) {
            parent_object = storage_id;
        }

        /* Check if we know about the parent object. If we don't, it's an error. */
        auto * const parentobj = m_object_database.GetObjectById(parent_object);
        R_UNLESS(parentobj != nullptr, haze::ResultInvalidObjectId());

        /* Make a new object with the intended name. */
        PtpNewObjectInfo new_object_info;
        new_object_info.storage_id       = static_cast<StorageId>(this->GetStorageForObject(parent_object));
        new_object_info.parent_object_id = parent_object == storage_id ? 0 : parent_object;

        /* Create the object in the database. */
        PtpObject *obj;
        R_TRY(m_object_database.CreateOrFindObject(parentobj->GetName(), m_buffers->filename_string_buffer, parentobj->GetObjectId(), std::addressof(obj)));

        /* Ensure we maintain a clean state on failure. */
        ON_RESULT_FAILURE { m_object_database.DeleteObject(obj); };

        /* Register the object with a new ID. */
        m_object_database.RegisterObject(obj);
        new_object_info.object_id = obj->GetObjectId();

        /* Create the object on the filesystem. */
        if (info.object_format == PtpObjectFormatCode_Association) {
            R_TRY(m_fs.CreateDirectory(obj->GetName()));
            m_send_object_id = 0;
        } else {
            R_TRY(m_fs.CreateFile(obj->GetName(), 0, 0));
            m_send_object_id = new_object_info.object_id;
        }

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok, new_object_info));
    }

    Result PtpResponder::SendObject(PtpDataParser &rdp) {
        /* Reset SendObject object ID on exit. */
        ON_SCOPE_EXIT { m_send_object_id = 0; };

        R_TRY(rdp.Finalize());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,  haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,         haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id, haze::ResultOperationNotSupported());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(m_send_object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Lock the object as a file. */
        FsFile file;
        R_TRY(m_fs.OpenFile(obj->GetName(), FsOpenMode_Write | FsOpenMode_Append, std::addressof(file)));

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { m_fs.CloseFile(std::addressof(file)); };

        /* Truncate the file after locking for write. */
        s64 offset = 0;
        R_TRY(m_fs.SetFileSize(std::addressof(file), 0));

        /* Expand to the needed size. */
        if (data_header.length > sizeof(PtpUsbBulkContainer)) {
            R_TRY(m_fs.SetFileSize(std::addressof(file), data_header.length - sizeof(PtpUsbBulkContainer)));
        }

        /* Begin writing to the filesystem. */
        while (true) {
            /* Read as many bytes as we can. */
            u32 bytes_received;
            const Result read_res = dp.ReadBuffer(m_buffers->file_system_data_buffer, FsBufferSize, std::addressof(bytes_received));

            /* Write to the file. */
            R_TRY(m_fs.WriteFile(std::addressof(file), offset, m_buffers->file_system_data_buffer, bytes_received, 0));

            offset += bytes_received;

            /* If we received fewer bytes than the batch size, we're done. */
            if (haze::ResultEndOfTransmission::Includes(read_res)) {
                break;
            }

            R_TRY(read_res);
        }

        /* Truncate the file to the received size. */
        R_TRY(m_fs.SetFileSize(std::addressof(file), offset));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::DeleteObject(PtpDataParser &dp) {
        /* Get the object ID and flush the request packet. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Disallow deleting any storage root. */
        R_UNLESS(!this->IsStorageRoot(object_id), haze::ResultInvalidObjectId());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Figure out what type of object this is. */
        FsDirEntryType entry_type;
        R_TRY(m_fs.GetEntryType(obj->GetName(), std::addressof(entry_type)));

        /* Remove the object from the filesystem. */
        if (entry_type == FsDirEntryType_Dir) {
            R_TRY(m_fs.DeleteDirectoryRecursively(obj->GetName()));
        } else {
            R_TRY(m_fs.DeleteFile(obj->GetName()));
        }

        /* Remove the object from the database. */
        m_object_database.DeleteObject(obj);

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

}
