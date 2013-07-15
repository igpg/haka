------------------------------------
-- HTTP Policy
------------------------------------

-- add custom user-agent
haka.rule {
	hooks = {"http-request"},
	eval = function (self, http)
		http.request.headers["User-Agent"] = "Haka User-Agent"
	end
}

-- warn if method is different than get and post 
haka.rule {
	hooks = {"http-request"},
	eval = function (self, http)
		local method = http.request.method:lower()
		if method ~= 'get' and method ~= 'post' then
			haka.log.warning("filter", "forbidden http method '%s'", method)
		end
	end
}
