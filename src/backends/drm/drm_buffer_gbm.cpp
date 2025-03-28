/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2017 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_buffer_gbm.h"
#include "gbm_surface.h"

#include "config-kwin.h"
#include "drm_backend.h"
#include "drm_gpu.h"
#include "kwineglutils_p.h"
#include "logging.h"
#include "wayland/clientbuffer.h"
#include "wayland/linuxdmabufv1clientbuffer.h"

#include <cerrno>
#include <drm_fourcc.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace KWin
{

static std::array<uint32_t, 4> getHandles(gbm_bo *bo)
{
    std::array<uint32_t, 4> ret;
    int i = 0;
    for (; i < gbm_bo_get_plane_count(bo); i++) {
        ret[i] = gbm_bo_get_handle(bo).u32;
    }
    for (; i < 4; i++) {
        ret[i] = 0;
    }
    return ret;
}

static std::array<uint32_t, 4> getStrides(gbm_bo *bo)
{
    std::array<uint32_t, 4> ret;
    int i = 0;
    for (; i < gbm_bo_get_plane_count(bo); i++) {
        ret[i] = gbm_bo_get_stride_for_plane(bo, i);
    }
    for (; i < 4; i++) {
        ret[i] = 0;
    }
    return ret;
}

static std::array<uint32_t, 4> getOffsets(gbm_bo *bo)
{
    std::array<uint32_t, 4> ret;
    int i = 0;
    for (; i < gbm_bo_get_plane_count(bo); i++) {
        ret[i] = gbm_bo_get_offset(bo, i);
    }
    for (; i < 4; i++) {
        ret[i] = 0;
    }
    return ret;
}

GbmBuffer::GbmBuffer(DrmGpu *gpu, gbm_bo *bo, const std::shared_ptr<GbmSurface> &surface)
    : DrmGpuBuffer(gpu, QSize(gbm_bo_get_width(bo), gbm_bo_get_height(bo)), gbm_bo_get_format(bo), gbm_bo_get_modifier(bo), getHandles(bo), getStrides(bo), getOffsets(bo), gbm_bo_get_plane_count(bo))
    , m_bo(bo)
    , m_surface(surface)
{
}

GbmBuffer::GbmBuffer(DrmGpu *gpu, gbm_bo *bo)
    : DrmGpuBuffer(gpu, QSize(gbm_bo_get_width(bo), gbm_bo_get_height(bo)), gbm_bo_get_format(bo), gbm_bo_get_modifier(bo), getHandles(bo), getStrides(bo), getOffsets(bo), gbm_bo_get_plane_count(bo))
    , m_bo(bo)
{
}

GbmBuffer::GbmBuffer(DrmGpu *gpu, gbm_bo *bo, KWaylandServer::LinuxDmaBufV1ClientBuffer *clientBuffer)
    : DrmGpuBuffer(gpu, QSize(gbm_bo_get_width(bo), gbm_bo_get_height(bo)), gbm_bo_get_format(bo), gbm_bo_get_modifier(bo), getHandles(bo), getStrides(bo), getOffsets(bo), gbm_bo_get_plane_count(bo))
    , m_bo(bo)
    , m_clientBuffer(clientBuffer)
{
    m_clientBuffer->ref();
}

GbmBuffer::~GbmBuffer()
{
    if (m_clientBuffer) {
        m_clientBuffer->unref();
    }
    if (m_mapping) {
        gbm_bo_unmap(m_bo, m_mapping);
    }
    if (m_surface) {
        m_surface->releaseBuffer(this);
    } else {
        gbm_bo_destroy(m_bo);
    }
}

gbm_bo *GbmBuffer::bo() const
{
    return m_bo;
}

void *GbmBuffer::mappedData() const
{
    return m_data;
}

KWaylandServer::ClientBuffer *GbmBuffer::clientBuffer() const
{
    return m_clientBuffer;
}

bool GbmBuffer::map(uint32_t flags)
{
    if (m_data) {
        return true;
    }
    uint32_t stride = m_strides[0];
    m_data = gbm_bo_map(m_bo, 0, 0, m_size.width(), m_size.height(), flags, &stride, &m_mapping);
    return m_data;
}

void GbmBuffer::createFds()
{
#if HAVE_GBM_BO_GET_FD_FOR_PLANE
    for (uint32_t i = 0; i < m_planeCount; i++) {
        m_fds[i] = gbm_bo_get_fd_for_plane(m_bo, i);
        if (m_fds[i] == -1) {
            for (uint32_t i2 = 0; i2 < i; i2++) {
                close(m_fds[i2]);
                m_fds[i2] = -1;
            }
            return;
        }
    }
    return;
#else
    if (m_planeCount > 1) {
        return;
    }
    m_fds[0] = gbm_bo_get_fd(m_bo);
#endif
}

std::shared_ptr<GbmBuffer> GbmBuffer::importBuffer(DrmGpu *gpu, KWaylandServer::LinuxDmaBufV1ClientBuffer *clientBuffer)
{
    const auto planes = clientBuffer->planes();
    gbm_bo *bo;
    if (planes.first().modifier != DRM_FORMAT_MOD_INVALID || planes.first().offset > 0 || planes.count() > 1) {
        gbm_import_fd_modifier_data data = {};
        data.format = clientBuffer->format();
        data.width = (uint32_t)clientBuffer->size().width();
        data.height = (uint32_t)clientBuffer->size().height();
        data.num_fds = planes.count();
        data.modifier = planes.first().modifier;
        for (int i = 0; i < planes.count(); i++) {
            data.fds[i] = planes[i].fd;
            data.offsets[i] = planes[i].offset;
            data.strides[i] = planes[i].stride;
        }
        bo = gbm_bo_import(gpu->gbmDevice(), GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_SCANOUT);
    } else {
        const auto &plane = planes.first();
        gbm_import_fd_data data = {};
        data.fd = plane.fd;
        data.width = (uint32_t)clientBuffer->size().width();
        data.height = (uint32_t)clientBuffer->size().height();
        data.stride = plane.stride;
        data.format = clientBuffer->format();
        bo = gbm_bo_import(gpu->gbmDevice(), GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT);
    }
    if (bo) {
        return std::make_shared<GbmBuffer>(gpu, bo, clientBuffer);
    } else {
        return nullptr;
    }
}

std::shared_ptr<GbmBuffer> GbmBuffer::importBuffer(DrmGpu *gpu, GbmBuffer *buffer, uint32_t flags)
{
    const auto fds = buffer->fds();
    if (fds[0] == -1) {
        return nullptr;
    }
    const auto strides = buffer->strides();
    const auto offsets = buffer->offsets();
    gbm_import_fd_modifier_data data = {
        .width = (uint32_t)buffer->size().width(),
        .height = (uint32_t)buffer->size().height(),
        .format = buffer->format(),
        .num_fds = (uint32_t)buffer->planeCount(),
        .fds = {},
        .strides = {},
        .offsets = {},
        .modifier = buffer->modifier(),
    };
    for (uint32_t i = 0; i < data.num_fds; i++) {
        data.fds[i] = fds[i];
        data.strides[i] = strides[i];
        data.offsets[i] = offsets[i];
    }
    gbm_bo *bo = gbm_bo_import(gpu->gbmDevice(), GBM_BO_IMPORT_FD_MODIFIER, &data, flags);
    if (bo) {
        return std::make_shared<GbmBuffer>(gpu, bo);
    } else {
        return nullptr;
    }
}
}
