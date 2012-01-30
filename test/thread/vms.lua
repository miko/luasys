#!/usr/bin/env lua

local string = require"string"
local sys = require"sys"

local thread = sys.thread

thread.init()

assert(thread.self():wait())

-- Pipe
local work_pipe = thread.pipe()

-- Consumer VM-Thread
do
    local function consume(work_pipe)
	local sys = require"sys"
	local thread = sys.thread

	while true do
	    local i, s = work_pipe:get(200)
	    if not i then break end
	    print(i, s)
	    thread.sleep(200)
	end
    end

    assert(thread.runvm(string.dump(consume), work_pipe))
end

-- Producer VM-Thread
do
    local function produce(work_pipe)
	local sys = require"sys"
	local thread = sys.thread

	for i = 1, 10 do
	    work_pipe:put(i, (i % 2 == 0) and "even" or "odd")
	    thread.sleep(100)
	end
    end

    assert(thread.runvm(string.dump(produce), work_pipe))
end

-- Wait VM-Threads termination
assert(thread.self():wait())
