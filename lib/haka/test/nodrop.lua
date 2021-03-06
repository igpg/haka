-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

-- Basic test that will test packet

haka.policy {
	on = haka.dissectors.packet.policies.next_dissector,
	name = "don't drop",
	action = haka.policy.accept
}

haka.rule {
	on = haka.dissectors.packet.events.receive_packet,
	eval = function (pkt)
		debug.pprint(pkt, { hide = { debug.hide_underscore, debug.hide_function } })
	end
}
