// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native control endpoint from UniXNU-sheng WifiProbe.cpp.
struct ControlClient {uint64_t session; uint32_t unit;};
kern_ctl_ref gControlRef;
IOLock *gControlLock;
ControlClient *gControlClient;
errno_t controlError(int status) {
    switch (status) {
        case NW_CONTROL_OK: return 0;
        case NW_CONTROL_INVALID: return EINVAL;
        case NW_CONTROL_BUSY: return EWOULDBLOCK;
        case NW_CONTROL_NOT_READY: return ENXIO;
        case NW_CONTROL_AUTH_FAILED: return EACCES;
        case NW_CONTROL_TIMED_OUT: return ETIMEDOUT;
        case NW_CONTROL_UNSUPPORTED: return EOPNOTSUPP;
        case NW_CONTROL_NO_MEMORY: return ENOBUFS;
        case NW_CONTROL_NOT_FOUND: return ENOENT;
        case NW_CONTROL_STALE: return ESTALE;
        default: return EIO;
    }
}
errno_t controlConnect(kern_ctl_ref, sockaddr_ctl *address, void **information) {
    auto *owner = static_cast<ControlClient *>(IOMalloc(sizeof(ControlClient)));
    if (!owner) return ENOMEM;
    const int status = nw_wifi_control_attach(&owner->session);
    if (status) {IOFree(owner, sizeof(*owner)); return controlError(status);}
    owner->unit = address->sc_unit;
    IOLockLock(gControlLock); gControlClient = owner; IOLockUnlock(gControlLock);
    *information = owner; return 0;
}
errno_t controlDisconnect(kern_ctl_ref, uint32_t, void *information) {
    // XNU invokes disconnect after a failed connect, with no client context.
    if (!information) return 0;
    auto *owner = static_cast<ControlClient *>(information);
    nw_wifi_control_detach(owner->session);
    IOLockLock(gControlLock); if (gControlClient == owner) gControlClient = nullptr; IOLockUnlock(gControlLock);
    IOFree(owner, sizeof(*owner)); return 0;
}
int controlReadMbuf(void *source, void *destination, size_t length) {
    return mbuf_copydata(static_cast<mbuf_t>(source), 0, length, destination);
}
errno_t controlReceive(kern_ctl_ref, uint32_t, void *information, mbuf_t packet, int) {
    auto *owner = static_cast<ControlClient *>(information);
    const int status = nw_wifi_control_receive(owner->session, mbuf_pkthdr_len(packet), controlReadMbuf, packet);
    mbuf_freem(packet); return controlError(status);
}
void controlReceived(kern_ctl_ref, uint32_t, void *information, int) {
    nw_wifi_control_writable(static_cast<ControlClient *>(information)->session);
}
errno_t controlGetOption(kern_ctl_ref, uint32_t, void *information, int option, void *bytes, size_t *length) {
    if (option != NW_CONTROL_GET_SESSION) return ENOPROTOOPT;
    if (!length) return EINVAL;
    if (!bytes || *length < sizeof(uint64_t)) {*length = sizeof(uint64_t); return EMSGSIZE;}
    const uint64_t session = static_cast<ControlClient *>(information)->session;
    memcpy(bytes, &session, sizeof(session)); *length = sizeof(session); return 0;
}
int controlSend(void *, uint64_t session, const void *bytes, size_t length) {
    IOLockLock(gControlLock);
    const bool active = gControlClient && gControlClient->session == session;
    const uint32_t unit = active ? gControlClient->unit : 0;
    IOLockUnlock(gControlLock);
    if (!active) return NW_CONTROL_SEND_CLOSED;
    // No adapter lock is held while the socket layer takes its own locks.
    const errno_t result = ctl_enqueuedata(gControlRef, unit, const_cast<void *>(bytes), length, CTL_DATA_EOR);
    if (!result) return NW_CONTROL_SEND_OK;
    if (result == ENOBUFS || result == EWOULDBLOCK) return NW_CONTROL_SEND_AGAIN;
    return NW_CONTROL_SEND_CLOSED;
}
void controlWake(void *) {marble_wifi_wake();}
int controlStart() {
    if (gControlRef) return 0;
    if (!gControlLock) gControlLock = IOLockAlloc();
    if (!gControlLock) return ENOMEM;
    kern_ctl_reg registration{};
    snprintf(registration.ctl_name, sizeof(registration.ctl_name), "%s", NW_CONTROL_NAME);
    registration.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
    registration.ctl_sendsize = 256 * 1024; registration.ctl_recvsize = 512 * 1024;
    registration.ctl_connect = controlConnect; registration.ctl_disconnect = controlDisconnect;
    registration.ctl_send = controlReceive; registration.ctl_getopt = controlGetOption; registration.ctl_rcvd = controlReceived;
    const errno_t result = ctl_register(&registration, &gControlRef);
    if (result) return result;
    const nw_control_transport transport{nullptr, controlSend, controlWake};
    const int status = nw_wifi_control_transport_set(&transport);
    if (status) {ctl_deregister(gControlRef); gControlRef = nullptr; return controlError(status);}
    return 0;
}

