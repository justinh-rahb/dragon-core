#include "dc_peer.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    CHECK(dc_peer_kind_from_id("dragonbreath-e51d") == DC_PEER_KIND_BREATH);
    CHECK(dc_peer_kind_from_id("dragonvent-e51d") == DC_PEER_KIND_VENT);
    CHECK(dc_peer_kind_from_id("dragonwheeze-e51d") == DC_PEER_KIND_WHEEZE);
    CHECK(dc_peer_kind_from_id("dragontouch-e51d") == DC_PEER_KIND_TOUCH);
    CHECK(dc_peer_kind_from_id("dragonstatus-e51d") == DC_PEER_KIND_STATUS);
    CHECK(dc_peer_kind_from_id("dragonstatus") == DC_PEER_KIND_UNKNOWN);
    CHECK(dc_peer_kind_from_id("notdragon-status-e51d") == DC_PEER_KIND_UNKNOWN);
    CHECK(dc_peer_kind_from_id(NULL) == DC_PEER_KIND_UNKNOWN);

    CHECK(DC_PEER_CAP_LIGHTING < 8);
    CHECK((DC_PEER_CAP_BIT(DC_PEER_CAP_LIGHTING) &
           DC_PEER_CAP_BIT(DC_PEER_CAP_ANNOUNCE)) == 0);
    CHECK(sizeof(dc_peer_lighting_t) == 16);
    CHECK(sizeof(dc_peer_lighting_t) <= DC_PEER_PAYLOAD_MAX);

    puts("dc_peer contract tests passed");
    return 0;
}
