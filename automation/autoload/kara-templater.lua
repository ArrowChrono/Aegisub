--[[
 Copyright (c) 2007, Niels Martin Hansen, Rodrigo Braz Monteiro
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   * Neither the name of the Aegisub Group nor the names of its contributors
     may be used to endorse or promote products derived from this software
     without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
]]

-- Aegisub Automation 4 Lua karaoke templater tool
-- Parse and apply a karaoke effect written in ASS karaoke template language
-- See help file and wiki for more information on this

local tr = aegisub.gettext

script_name = tr"Karaoke Templater"
script_description = tr"Macro and export filter to apply karaoke effects using the template language"
script_author = "Niels Martin Hansen"
script_version = "2.1.7"


include("karaskel.lua")

local is_template_debug_enabled = aegisub.__is_debug_template_enabled or function() return false end
local set_template_debug_context = aegisub.__set_debug_template_context or function() end
local template_debug_sequence = 0
local template_debug_enabled = false

local function refresh_template_debug_enabled()
	template_debug_enabled = is_template_debug_enabled()
	return template_debug_enabled
end

local function clear_template_debug_context()
	if template_debug_enabled then
		set_template_debug_context(nil)
	end
end

local function template_debug_next_id()
	template_debug_sequence = template_debug_sequence + 1
	return template_debug_sequence
end

local function template_debug_append_unique(list, value)
	if value == nil then return end
	for _, existing in ipairs(list) do
		if existing == value then
			return
		end
	end
	table.insert(list, value)
end

local function template_debug_copy_array(items)
	if type(items) ~= "table" then return nil end
	local copy = {}
	for i, item in ipairs(items) do
		if type(item) == "table" then
			local itemcopy = {}
			for k, v in pairs(item) do
				itemcopy[k] = v
			end
			copy[i] = itemcopy
		else
			copy[i] = item
		end
	end
	return copy
end

local function template_debug_copy_map(items)
	if type(items) ~= "table" then return nil end
	local copy = {}
	for k, v in pairs(items) do
		copy[k] = v
	end
	return copy
end

local function template_debug_get_state(tenv)
	if not template_debug_enabled or not tenv then return nil end
	local state = tenv.__aegi_template_debug
	if not state then
		state = {
			generated_count = 0,
			last_generated_line = nil,
		}
		tenv.__aegi_template_debug = state
	end
	return state
end

local function template_debug_enter(tenv, patch)
	local state = template_debug_get_state(tenv)
	if not state or not patch then return nil end
	local previous = {}
	for k, v in pairs(patch) do
		previous[k] = state[k]
		state[k] = v
	end
	return previous
end

local function template_debug_leave(tenv, previous)
	if not previous then return end
	local state = template_debug_get_state(tenv)
	if not state then return end
	for k, v in pairs(previous) do
		state[k] = v
	end
end

local function template_debug_ensure_info(template, seed)
	if not template_debug_enabled or not template then return nil end
	local info = rawget(template, "debug_info")
	if not info then
		info = {
			owner_script = "kara-templater.lua",
			template_debug_id = template_debug_next_id(),
			template_kind = nil,
			template_kinds = {},
			fragment_kind = nil,
			source_line_index = nil,
			source_line_indices = {},
			source_fragments = {},
			source_style = nil,
			source_effect = nil,
			source_text = nil,
		}
		template.debug_info = info
	end

	if seed then
		if seed.template_kind and not info.template_kind then
			info.template_kind = seed.template_kind
		end
		if seed.template_kind then
			template_debug_append_unique(info.template_kinds, seed.template_kind)
		end
		if seed.fragment_kind and not info.fragment_kind then
			info.fragment_kind = seed.fragment_kind
		end
		if seed.source_line_index and not info.source_line_index then
			info.source_line_index = seed.source_line_index
		end
		if seed.source_line_index then
			template_debug_append_unique(info.source_line_indices, seed.source_line_index)
		end
		if seed.source_style then
			info.source_style = seed.source_style
		end
		if seed.source_effect then
			info.source_effect = seed.source_effect
		end
		if seed.source_text then
			info.source_text = seed.source_text
		end
		if seed.source_fragment_kind then
			table.insert(info.source_fragments, {
				source_line_index = seed.source_line_index,
				fragment_kind = seed.source_fragment_kind,
				text = seed.source_text,
				effect = seed.source_effect,
			})
		end
	end

	info.template_id = template.id or info.template_id
	info.template_style = template.style or info.template_style
	info.template_loops = template.loops or info.template_loops
	info.template_fx = template.fx or info.template_fx
	info.template_fxgroup = template.fxgroup or info.template_fxgroup
	info.template_is_line = template.isline or info.template_is_line
	info.template_perchar = template.perchar or info.template_perchar
	info.template_multi = template.multi or info.template_multi
	info.template_noblank = template.noblank or info.template_noblank

	return info
end

local function template_debug_line_view(line)
	if not line then return nil end
	return {
		i = line.i,
		class = line.class,
		layer = line.layer,
		style = line.style,
		actor = line.actor,
		effect = line.effect,
		comment = line.comment,
		text = line.text,
		start_time = line.start_time,
		end_time = line.end_time,
	}
end

local function template_debug_syl_view(syl)
	if not syl then return nil end
	return {
		i = syl.i,
		text = syl.text,
		text_stripped = syl.text_stripped,
		inline_fx = syl.inline_fx,
		start_time = syl.start_time,
		end_time = syl.end_time,
		duration = syl.duration,
		isfuri = syl.isfuri,
		left = syl.left,
		center = syl.center,
		right = syl.right,
		width = syl.width,
		height = syl.height,
	}
end

local function template_debug_highlight_view(highlight, highlight_index)
	if not highlight then return nil end
	return {
		i = highlight_index,
		start_time = highlight.start_time,
		end_time = highlight.end_time,
		duration = highlight.duration,
	}
end

local function template_debug_target_view(tenv, state)
	if not tenv then return nil end
	return {
		scope_kind = state and state.scope_kind or nil,
		orgline = template_debug_line_view(tenv.orgline),
		line = template_debug_line_view(tenv.line),
		syl = template_debug_syl_view(tenv.syl),
		basesyl = template_debug_syl_view(tenv.basesyl),
		highlight = template_debug_highlight_view(state and state.highlight or nil, state and state.highlight_index or nil),
		char = (state and state.char_index) and {
			i = state.char_index,
			text = state.char_text,
		} or nil,
	}
end

local function template_debug_record_generated_line(tenv, newline, extra)
	if not template_debug_enabled then return end
	local state = template_debug_get_state(tenv)
	if not state or not newline then return end
	state.generated_count = (state.generated_count or 0) + 1
	state.last_generated_line = {
		generated_index = state.generated_count,
		text = newline.text,
		style = newline.style,
		layer = newline.layer,
		effect = newline.effect,
		start_time = newline.start_time,
		end_time = newline.end_time,
		source_line_index = tenv and tenv.orgline and tenv.orgline.i or nil,
	}
	if extra then
		for k, v in pairs(extra) do
			state.last_generated_line[k] = v
		end
	end
end

local function update_template_debug_context(kind, template, tenv, extra)
	if not template_debug_enabled then return end
	local info = template_debug_ensure_info(template)
	local state = template_debug_get_state(tenv)
	local ctx = {
		kind = kind,
		template_loops = template and template.loops or nil,
		template_code = template and template.code or nil,
		template_text = template and template.t or nil,
		j = tenv and tenv.j or nil,
		maxj = tenv and tenv.maxj or nil,
		line_text = tenv and tenv.line and tenv.line.text or nil,
		line_style = tenv and tenv.line and tenv.line.style or nil,
		syl_text = tenv and tenv.syl and tenv.syl.text or nil,
		syl_i = tenv and tenv.syl and tenv.syl.i or nil,
		basesyl_text = tenv and tenv.basesyl and tenv.basesyl.text or nil,
		template_debug_id = info and info.template_debug_id or nil,
		template_kind = info and info.template_kind or nil,
		template_kinds = info and template_debug_copy_array(info.template_kinds) or nil,
		template_fragment_kind = info and info.fragment_kind or nil,
		template_id = info and info.template_id or nil,
		template_style = info and info.template_style or nil,
		template_source_line_index = info and info.source_line_index or nil,
		template_source_line_indices = info and template_debug_copy_array(info.source_line_indices) or nil,
		template_source_fragments = info and template_debug_copy_array(info.source_fragments) or nil,
		template_phase = state and state.phase or nil,
		debug_scope = state and state.scope_kind or nil,
		highlight_i = state and state.highlight_index or nil,
		char_i = state and state.char_index or nil,
		char_text = state and state.char_text or nil,
		template_identity = info and {
			owner_script = info.owner_script,
			template_debug_id = info.template_debug_id,
			template_kind = info.template_kind,
			template_kinds = template_debug_copy_array(info.template_kinds),
			fragment_kind = info.fragment_kind,
			template_id = info.template_id,
			source_line_index = info.source_line_index,
			source_line_indices = template_debug_copy_array(info.source_line_indices),
		} or nil,
		template_source = info and {
			style = info.source_style,
			effect = info.source_effect,
			text = info.source_text,
			fragments = template_debug_copy_array(info.source_fragments),
		} or nil,
		target = template_debug_target_view(tenv, state),
		generated = state and {
			count = state.generated_count or 0,
			last_line = template_debug_copy_map(state.last_generated_line),
		} or nil,
	}

	if extra then
		for k, v in pairs(extra) do
			ctx[k] = v
		end
	end

	set_template_debug_context(ctx)
end


-- Find and parse/prepare all karaoke template lines
function parse_templates(meta, styles, subs)
	template_debug_sequence = 0
	refresh_template_debug_enabled()
	local templates = { once = {}, line = {}, syl = {}, char = {}, furi = {}, styles = {} }
	local i = 1
	while i <= #subs do
		aegisub.progress.set((i-1) / #subs * 100)
		local line_index = i
		local l = subs[i]
		i = i + 1
		if l.class == "dialogue" and l.comment then
			local fx, mods = string.headtail(l.effect)
			fx = fx:lower()
			if fx == "code" then
				parse_code(meta, styles, l, templates, mods, line_index)
			elseif fx == "template" then
				parse_template(meta, styles, l, templates, mods, line_index)
			end
			templates.styles[l.style] = true
		elseif l.class == "dialogue" and l.effect == "fx" then
			-- this is a previously generated effect line, remove it
			i = i - 1
			subs.delete(i)
		end
	end
	aegisub.progress.set(100)
	return templates
end

function parse_code(meta, styles, line, templates, mods, line_index)
	local template = {
		code = line.text,
		loops = 1,
		style = line.style
	}
	template_debug_ensure_info(template, {
		fragment_kind = "code-template",
		source_line_index = line_index,
		source_style = line.style,
		source_effect = line.effect,
		source_text = line.text,
		source_fragment_kind = "code-template",
	})
	local inserted = false

	local rest = mods
	while rest ~= "" do
		local m, t = string.headtail(rest)
		rest = t
		m = m:lower()
		if m == "once" then
			aegisub.debug.out(5, "Found run-once code line: %s\n", line.text)
			template_debug_ensure_info(template, { template_kind = "once" })
			table.insert(templates.once, template)
			inserted = true
		elseif m == "line" then
			aegisub.debug.out(5, "Found per-line code line: %s\n", line.text)
			template_debug_ensure_info(template, { template_kind = "line" })
			table.insert(templates.line, template)
			inserted = true
		elseif m == "syl" then
			aegisub.debug.out(5, "Found per-syl code line: %s\n", line.text)
			template_debug_ensure_info(template, { template_kind = "syl" })
			table.insert(templates.syl, template)
			inserted = true
		elseif m == "furi" then
			aegisub.debug.out(5, "Found per-syl code line: %s\n", line.text)
			template_debug_ensure_info(template, { template_kind = "furi" })
			table.insert(templates.furi, template)
			inserted = true
		elseif m == "all" then
			template.style = nil
		elseif m == "noblank" then
			template.noblank = true
		elseif m == "repeat" or m == "loop" then
			local times, t = string.headtail(rest)
			template.loops = tonumber(times)
			if not template.loops then
				aegisub.debug.out(3, "Failed reading this repeat-count to a number: %s\nIn template code line: %s\nEffect field: %s\n\n", times, line.text, line.effect)
				template.loops = 1
			else
				rest = t
			end
		else
			aegisub.debug.out(3, "Unknown modifier in code template: %s\nIn template code line: %s\nEffect field: %s\n\n", m, line.text, line.effect)
		end
	end

	if not inserted then
		aegisub.debug.out(5, "Found implicit run-once code line: %s\n", line.text)
		template_debug_ensure_info(template, { template_kind = "once" })
		table.insert(templates.once, template)
	end
end

-- List of reserved words that can't be used as "line" template identifiers
template_modifiers = {
	"pre-line", "line", "syl", "furi", "char", "all", "repeat", "loop",
	"notext", "keeptags", "noblank", "multi", "fx", "fxgroup"
}

function parse_template(meta, styles, line, templates, mods, line_index)
	local template = {
		t = "",
		pre = "",
		style = line.style,
		loops = 1,
		layer = line.layer,
		addtext = true,
		keeptags = false,
		fxgroup = nil,
		fx = nil,
		multi = false,
		isline = false,
		perchar = false,
		noblank = false
	}
	local inserted = false

	local rest = mods
	while rest ~= "" do
		local m, t = string.headtail(rest)
		rest = t
		m = m:lower()
		if (m == "pre-line" or m == "line") and not inserted then
			aegisub.debug.out(5, "Found line template '%s'\n", line.text)
			-- should really fail if already inserted
			local id, t = string.headtail(rest)
			id = id:lower()
			-- check that it really is an identifier and not a keyword
			for _, kw in pairs(template_modifiers) do
				if id == kw then
					id = nil
					break
				end
			end
			if id == "" then
				id = nil
			end
			if id then
				rest = t
			end
			-- get old template if there is one
			if id and templates.line[id] then
				template = templates.line[id]
			elseif id then
				template.id = id
				templates.line[id] = template
			else
				table.insert(templates.line, template)
			end
			inserted = true
			template.isline = true
			template_debug_ensure_info(template, {
				template_kind = "line",
				fragment_kind = "text-template",
				source_line_index = line_index,
				source_style = line.style,
				source_effect = line.effect,
				source_text = line.text,
				source_fragment_kind = m,
			})
			-- apply text to correct string
			if m == "line" then
				template.t = template.t .. line.text
			else -- must be pre-line
				template.pre = template.pre .. line.text
			end
		elseif m == "syl" and not template.isline then
			template_debug_ensure_info(template, {
				template_kind = "syl",
				fragment_kind = "text-template",
				source_line_index = line_index,
				source_style = line.style,
				source_effect = line.effect,
				source_text = line.text,
				source_fragment_kind = "syl",
			})
			table.insert(templates.syl, template)
			inserted = true
		elseif m == "furi" and not template.isline then
			template_debug_ensure_info(template, {
				template_kind = "furi",
				fragment_kind = "text-template",
				source_line_index = line_index,
				source_style = line.style,
				source_effect = line.effect,
				source_text = line.text,
				source_fragment_kind = "furi",
			})
			table.insert(templates.furi, template)
			inserted = true
		elseif (m == "pre-line" or m == "line") and inserted then
			aegisub.debug.out(2, "Unable to combine %s class templates with other template classes\n\n", m)
		elseif (m == "syl" or m == "furi") and template.isline then
			aegisub.debug.out(2, "Unable to combine %s class template lines with line or pre-line classes\n\n", m)
		elseif m == "all" then
			template.style = nil
		elseif m == "repeat" or m == "loop" then
			local times, t = string.headtail(rest)
			template.loops = tonumber(times)
			if not template.loops then
				aegisub.debug.out(3, "Failed reading this repeat-count to a number: %s\nIn template line: %s\nEffect field: %s\n\n", times, line.text, line.effect)
				template.loops = 1
			else
				rest = t
			end
		elseif m == "notext" then
			template.addtext = false
		elseif m == "keeptags" then
			template.keeptags = true
		elseif m == "multi" then
			template.multi = true
		elseif m == "char" then
			template.perchar = true
		elseif m == "noblank" then
			template.noblank = true
		elseif m == "fx" then
			local fx, t = string.headtail(rest)
			if fx ~= "" then
				template.fx = fx
				rest = t
			else
				aegisub.debug.out(3, "No fx name following fx modifier\nIn template line: %s\nEffect field: %s\n\n", line.text, line.effect)
				template.fx = nil
			end
		elseif m == "fxgroup" then
			local fx, t = string.headtail(rest)
			if fx ~= "" then
				template.fxgroup = fx
				rest = t
			else
				aegisub.debug.out(3, "No fxgroup name following fxgroup modifier\nIn template linee: %s\nEffect field: %s\n\n", line.text, line.effect)
				template.fxgroup = nil
			end
		else
			aegisub.debug.out(3, "Unknown modifier in template: %s\nIn template line: %s\nEffect field: %s\n\n", m, line.text, line.effect)
		end
	end

	if not inserted then
		template_debug_ensure_info(template, {
			template_kind = "syl",
			fragment_kind = "text-template",
			source_line_index = line_index,
			source_style = line.style,
			source_effect = line.effect,
			source_text = line.text,
			source_fragment_kind = "default-syl",
		})
		table.insert(templates.syl, template)
	end
	if not template.isline then
		template.t = line.text
	end
end

-- Iterator function, return all templates that apply to the given line
function matching_templates(templates, line, tenv)
	local lastkey = nil
	local function test_next()
		local k, t = next(templates, lastkey)
		lastkey = k
		if not t then
			return nil
		elseif (t.style == line.style or not t.style) and
				(not t.fxgroup or
				(t.fxgroup and tenv.fxgroup[t.fxgroup] ~= false)) then
			return t
		else
			return test_next()
		end
	end
	return test_next
end

-- Iterator function, run a loop using tenv.j and tenv.maxj as loop controllers
function template_loop(tenv, initmaxj)
	local oldmaxj = initmaxj
	tenv.maxj = initmaxj
	tenv.j = 0
	local function itor()
		if tenv.j >= tenv.maxj or aegisub.progress.is_cancelled() then
			return nil
		else
			tenv.j = tenv.j + 1
			if oldmaxj ~= tenv.maxj then
				aegisub.debug.out(5, "Number of loop iterations changed from %d to %d\n", oldmaxj, tenv.maxj)
				oldmaxj = tenv.maxj
			end
			return tenv.j, tenv.maxj
		end
	end
	return itor
end


-- Apply the templates
function apply_templates(meta, styles, subs, templates)
	refresh_template_debug_enabled()
	-- the environment the templates will run in
	local tenv = {
		meta = meta,
		-- put in some standard libs
		string = string,
		math = math,
		_G = _G
	}
	tenv.tenv = tenv
	if template_debug_enabled then
		tenv.__aegi_template_debug = {
			phase = "initializing",
			scope_kind = nil,
			highlight = nil,
			highlight_index = nil,
			char_index = nil,
			char_text = nil,
			generated_count = 0,
			last_generated_line = nil,
		}
	end

	-- Define helper functions in tenv

	tenv.retime = function(mode, addstart, addend)
		local line, syl = tenv.line, tenv.syl
		local newstart, newend = line.start_time, line.end_time
		addstart = addstart or 0
		addend = addend or 0
		if mode == "syl" then
			newstart = line.start_time + syl.start_time + addstart
			newend = line.start_time + syl.end_time + addend
		elseif mode == "presyl" then
			newstart = line.start_time + syl.start_time + addstart
			newend = line.start_time + syl.start_time + addend
		elseif mode == "postsyl" then
			newstart = line.start_time + syl.end_time + addstart
			newend = line.start_time + syl.end_time + addend
		elseif mode == "line" then
			newstart = line.start_time + addstart
			newend = line.end_time + addend
		elseif mode == "preline" then
			newstart = line.start_time + addstart
			newend = line.start_time + addend
		elseif mode == "postline" then
			newstart = line.end_time + addstart
			newend = line.end_time + addend
		elseif mode == "start2syl" then
			newstart = line.start_time + addstart
			newend = line.start_time + syl.start_time + addend
		elseif mode == "syl2end" then
			newstart = line.start_time + syl.end_time + addstart
			newend = line.end_time + addend
		elseif mode == "set" or mode == "abs" then
			newstart = addstart
			newend = addend
		elseif mode == "sylpct" then
			newstart = line.start_time + syl.start_time + addstart*syl.duration/100
			newend = line.start_time + syl.start_time + addend*syl.duration/100
		-- wishlist: something for fade-over effects,
		-- "time between previous line and this" and
		-- "time between this line and next"
		end
		line.start_time = newstart
		line.end_time = newend
		line.duration = newend - newstart
		return ""
	end

	tenv.fxgroup = {}

	tenv.relayer = function(layer)
		tenv.line.layer = layer
		return ""
	end

	tenv.restyle = function(style)
		tenv.line.style = style
		tenv.line.styleref = styles[style]
		return ""
	end

	tenv.maxloop = function(newmaxj)
		tenv.maxj = newmaxj
		return ""
	end
	tenv.maxloops = tenv.maxloop
	tenv.loopctl = function(newj, newmaxj)
		tenv.j = newj
		tenv.maxj = newmaxj
		return ""
	end

	tenv.recall = {}
	setmetatable(tenv.recall, {
		decorators = {},
		__call = function(tab, name, default)
			local decorator = getmetatable(tab).decorators[name]
			if decorator then
				name = decorator(tostring(name))
			end
			aegisub.debug.out(5, "Recalling '%s'\n", name)
			return tab[name] or default
		end,
		decorator_line = function(name)
			return string.format("_%s_%s", tostring(tenv.orgline), name)
		end,
		decorator_syl = function(name)
			return string.format("_%s_%s", tostring(tenv.syl), name)
		end,
		decorator_basesyl = function(name)
			return string.format("_%s_%s", tostring(tenv.basesyl), name)
		end
	})
	tenv.remember = function(name, value, decorator)
		getmetatable(tenv.recall).decorators[name] = decorator
		if decorator then
			name = decorator(tostring(name))
		end
		aegisub.debug.out(5, "Remembering '%s' as '%s'\n", name, tostring(value))
		tenv.recall[name] = value
		return value
	end
	tenv.remember_line = function(name, value)
		return tenv.remember(name, value, getmetatable(tenv.recall).decorator_line)
	end
	tenv.remember_syl = function(name, value)
		return tenv.remember(name, value, getmetatable(tenv.recall).decorator_syl)
	end
	tenv.remember_basesyl = function(name, value)
		return tenv.remember(name, value, getmetatable(tenv.recall).decorator_basesyl)
	end
	tenv.remember_if = function(name, value, condition, decorator)
		if condition then
			return tenv.remember(name, value, decorator)
		end
		return value
	end

	-- run all run-once code snippets
	for k, t in pairs(templates.once) do
		assert(t.code, "WTF, a 'once' template without code?")
		local run_restore = template_debug_enter(tenv, {
			phase = "once-code",
			scope_kind = "once",
			highlight = nil,
			highlight_index = nil,
			char_index = nil,
			char_text = nil,
		})
		run_code_template(t, tenv)
		template_debug_leave(tenv, run_restore)
	end

	-- start processing lines
	local i, n = 0, #subs
	while i < n do
		aegisub.progress.set(i/n*100)
		i = i + 1
		local l = subs[i]
		if l.class == "dialogue" and ((l.effect == "" and not l.comment) or l.effect:match("[Kk]araoke")) then
			l.i = i
			l.comment = false
			karaskel.preproc_line(subs, meta, styles, l)
			if apply_line(meta, styles, subs, l, templates, tenv) then
				-- Some templates were applied to this line, make a karaoke timing line of it
				l.comment = true
				l.effect = "karaoke"
				subs[i] = l
			end
		end
	end
end

function set_ctx_syl(varctx, line, syl)
	varctx.sstart = syl.start_time
	varctx.send = syl.end_time
	varctx.sdur = syl.duration
	varctx.skdur = syl.duration / 10
	varctx.smid = syl.start_time + syl.duration / 2
	varctx["start"] = varctx.sstart
	varctx["end"] = varctx.send
	varctx.dur = varctx.sdur
	varctx.kdur = varctx.skdur
	varctx.mid = varctx.smid
	varctx.si = syl.i
	varctx.i = varctx.si
	varctx.sleft = math.floor(line.left + syl.left+0.5)
	varctx.scenter = math.floor(line.left + syl.center+0.5)
	varctx.sright = math.floor(line.left + syl.right+0.5)
	varctx.swidth = math.floor(syl.width + 0.5)
	if syl.isfuri then
		varctx.sbottom = varctx.ltop
		varctx.stop = math.floor(varctx.ltop - syl.height + 0.5)
		varctx.smiddle = math.floor(varctx.ltop - syl.height/2 + 0.5)
	else
		varctx.stop = varctx.ltop
		varctx.smiddle = varctx.lmiddle
		varctx.sbottom = varctx.lbottom
	end
	varctx.sheight = syl.height
	if line.halign == "left" then
		varctx.sx = math.floor(line.left + syl.left + 0.5)
	elseif line.halign == "center" then
		varctx.sx = math.floor(line.left + syl.center + 0.5)
	elseif line.halign == "right" then
		varctx.sx = math.floor(line.left + syl.right + 0.5)
	end
	if line.valign == "top" then
		varctx.sy = varctx.stop
	elseif line.valign == "middle" then
		varctx.sy = varctx.smiddle
	elseif line.valign == "bottom" then
		varctx.sy = varctx.sbottom
	end
	varctx.left = varctx.sleft
	varctx.center = varctx.scenter
	varctx.right = varctx.sright
	varctx.width = varctx.swidth
	varctx.top = varctx.stop
	varctx.middle = varctx.smiddle
	varctx.bottom = varctx.sbottom
	varctx.height = varctx.sheight
	varctx.x = varctx.sx
	varctx.y = varctx.sy
end

function apply_line(meta, styles, subs, line, templates, tenv)
	-- Tell whether any templates were applied to this line, needed to know whether the original line should be removed from input
	local applied_templates = false

	-- General variable replacement context
	local varctx = {
		layer = line.layer,
		lstart = line.start_time,
		lend = line.end_time,
		ldur = line.duration,
		lmid = line.start_time + line.duration/2,
		style = line.style,
		actor = line.actor,
		margin_l = ((line.margin_l > 0) and line.margin_l) or line.styleref.margin_l,
		margin_r = ((line.margin_r > 0) and line.margin_r) or line.styleref.margin_r,
		margin_t = ((line.margin_t > 0) and line.margin_t) or line.styleref.margin_t,
		margin_b = ((line.margin_b > 0) and line.margin_b) or line.styleref.margin_b,
		margin_v = ((line.margin_t > 0) and line.margin_t) or line.styleref.margin_t,
		syln = line.kara.n,
		li = line.i,
		lleft = math.floor(line.left+0.5),
		lcenter = math.floor(line.left + line.width/2 + 0.5),
		lright = math.floor(line.left + line.width + 0.5),
		lwidth = math.floor(line.width + 0.5),
		ltop = math.floor(line.top + 0.5),
		lmiddle = math.floor(line.middle + 0.5),
		lbottom = math.floor(line.bottom + 0.5),
		lheight = math.floor(line.height + 0.5),
		lx = math.floor(line.x+0.5),
		ly = math.floor(line.y+0.5)
	}

	tenv.orgline = line
	tenv.line = nil
	tenv.syl = nil
	tenv.basesyl = nil
	local debug_restore = template_debug_enter(tenv, {
		phase = "apply-line",
		scope_kind = "line",
		highlight = nil,
		highlight_index = nil,
		char_index = nil,
		char_text = nil,
	})

	-- Apply all line templates
	aegisub.debug.out(5, "Running line templates\n")
	for t in matching_templates(templates.line, line, tenv) do
		if aegisub.progress.is_cancelled() then break end

		-- Set varctx for per-line variables
		varctx["start"] = varctx.lstart
		varctx["end"] = varctx.lend
		varctx.dur = varctx.ldur
		varctx.kdur = math.floor(varctx.dur / 10)
		varctx.mid = varctx.lmid
		varctx.i = varctx.li
		varctx.left = varctx.lleft
		varctx.center = varctx.lcenter
		varctx.right = varctx.lright
		varctx.width = varctx.lwidth
		varctx.top = varctx.ltop
		varctx.middle = varctx.lmiddle
		varctx.bottom = varctx.lbottom
		varctx.height = varctx.lheight
		varctx.x = varctx.lx
		varctx.y = varctx.ly

		for j, maxj in template_loop(tenv, t.loops) do
			if t.code then
				aegisub.debug.out(5, "Code template, %s\n", t.code)
				tenv.line = line
				local run_restore = template_debug_enter(tenv, {
					phase = "line-code",
					scope_kind = "line",
				})
				-- Although run_code_template also performs template looping this works
				-- by "luck", since by the time the first loop of this outer loop completes
				-- the one run by run_code_template has already performed all iterations
				-- and has tenv.j and tenv.maxj in a loop-ending state, causing the outer
				-- loop to only ever run once.
				run_code_template(t, tenv)
				template_debug_leave(tenv, run_restore)
			else
				aegisub.debug.out(5, "Line template, pre = '%s', t = '%s'\n", t.pre, t.t)
				applied_templates = true
				local newline = table.copy(line)
				tenv.line = newline
				local run_restore = template_debug_enter(tenv, {
					phase = "line-text",
					scope_kind = "line",
				})
				newline.layer = t.layer
				newline.text = ""
				if t.pre ~= "" then
					newline.text = newline.text .. run_text_template(t.pre, tenv, varctx, t)
				end
				if t.t ~= "" then
					for i = 1, line.kara.n do
						local syl = line.kara[i]
						tenv.syl = syl
						tenv.basesyl = syl
						set_ctx_syl(varctx, line, syl)
						newline.text = newline.text .. run_text_template(t.t, tenv, varctx, t)
						if t.addtext then
							if t.keeptags then
								newline.text = newline.text .. syl.text
							else
								newline.text = newline.text .. syl.text_stripped
							end
						end
					end
				else
					-- hmm, no main template for the line... put original text in
					if t.keeptags then
						newline.text = newline.text .. line.text
					else
						newline.text = newline.text .. line.text_stripped
					end
				end
				newline.effect = "fx"
				subs.append(newline)
				template_debug_record_generated_line(tenv, newline, {
					template_debug_id = t.debug_info and t.debug_info.template_debug_id or nil,
					template_kind = t.debug_info and t.debug_info.template_kind or nil,
					scope_kind = "line",
				})
				update_template_debug_context("generated-line", t, tenv)
				template_debug_leave(tenv, run_restore)
			end
		end
	end
	aegisub.debug.out(5, "Done running line templates\n\n")

	-- Loop over syllables
	for i = 0, line.kara.n do
		if aegisub.progress.is_cancelled() then break end
		local syl = line.kara[i]

		aegisub.debug.out(5, "Applying templates to syllable: %s\n", syl.text)
		if apply_syllable_templates(syl, line, templates.syl, tenv, varctx, subs) then
			applied_templates = true
		end
	end

	-- Loop over furigana
	for i = 1, line.furi.n do
		if aegisub.progress.is_cancelled() then break end
		local furi = line.furi[i]

		aegisub.debug.out(5, "Applying templates to furigana: %s\n", furi.text)
		if apply_syllable_templates(furi, line, templates.furi, tenv, varctx, subs) then
			applied_templates = true
		end
	end

	template_debug_leave(tenv, debug_restore)
	return applied_templates
end

function run_code_template(template, tenv)
	local f, err = loadstring(template.code, "template code")
	if not f then
		update_template_debug_context("code-parse", template, tenv, { parse_error = err })
		aegisub.debug.out(2, "Failed to parse Lua code: %s\nCode that failed to parse: %s\n\n", err, template.code)
		clear_template_debug_context()
		aegisub.cancel()
	else
		local pcall = pcall
		setfenv(f, tenv)
		for j, maxj in template_loop(tenv, template.loops) do
			update_template_debug_context("code-run", template, tenv)
			local res, err = pcall(f)
			clear_template_debug_context()
			if not res then
				update_template_debug_context("code-error", template, tenv, { runtime_error = err })
				aegisub.debug.out(2, "Runtime error in template code: %s\nCode producing error: %s\n\n", err, template.code)
				clear_template_debug_context()
				aegisub.cancel()
			end
		end
	end
end

function run_text_template(template, tenv, varctx, debug_template)
	local res = template
	local debug_view = debug_template or { t = template }
	aegisub.debug.out(5, "Running text template '%s'\n", res)

	-- Replace the variables in the string (this is probably faster than using a custom function, but doesn't provide error reporting)
	if varctx then
		aegisub.debug.out(5, "Has varctx, replacing variables\n")
		local function var_replacer(varname)
			varname = string.lower(varname)
			aegisub.debug.out(5, "Found variable named '%s', ", varname)
			if varctx[varname] ~= nil then
				aegisub.debug.out(5, "it exists, value is '%s'\n", varctx[varname])
				return varctx[varname]
			else
				aegisub.debug.out(5, "doesn't exist\n")
				aegisub.debug.out(2, "Unknown variable name: %s\nIn karaoke template: %s\n\n", varname, template)
				return "$" .. varname
			end
		end
		res = string.gsub(res, "$([%a_]+)", var_replacer)
		aegisub.debug.out(5, "Done replacing variables, new template string is '%s'\n", res)
	end

	-- Function for evaluating expressions
	local function expression_evaluator(expression)
		f, err = loadstring(string.format("return (%s)", expression))
		if (err) ~= nil then
			update_template_debug_context("expression-parse", debug_view, tenv, { expression = expression, parse_error = err })
			aegisub.debug.out(2, "Error parsing expression: %s\nExpression producing error: %s\nTemplate with expression: %s\n\n", err, expression, template)
			clear_template_debug_context()
			aegisub.cancel()
		else
			setfenv(f, tenv)
			update_template_debug_context("expression-run", debug_view, tenv, { expression = expression })
			local res, val = pcall(f)
			clear_template_debug_context()
			if res then
				return val
			else
				update_template_debug_context("expression-error", debug_view, tenv, { expression = expression, runtime_error = val })
				aegisub.debug.out(2, "Runtime error in template expression: %s\nExpression producing error: %s\nTemplate with expression: %s\n\n", val, expression, template)
				clear_template_debug_context()
				aegisub.cancel()
			end
		end
	end
	-- Find and evaluate expressions
	aegisub.debug.out(5, "Now evaluating expressions\n")
	res = string.gsub(res , "!(.-)!", expression_evaluator)
	aegisub.debug.out(5, "After evaluation: %s\nDone handling template\n\n", res)

	return res
end

function apply_syllable_templates(syl, line, templates, tenv, varctx, subs)
	local applied = 0
	local debug_restore = template_debug_enter(tenv, {
		phase = syl.isfuri and "apply-furi" or "apply-syllable",
		scope_kind = syl.isfuri and "furi" or "syl",
		highlight = nil,
		highlight_index = nil,
		char_index = nil,
		char_text = nil,
	})

	-- Loop over all templates matching the line style
	for t in matching_templates(templates, line, tenv) do
		if aegisub.progress.is_cancelled() then break end

		tenv.syl = syl
		tenv.basesyl = syl
		set_ctx_syl(varctx, line, syl)

		applied = applied + apply_one_syllable_template(syl, line, t, tenv, varctx, subs, false, false)
	end

	template_debug_leave(tenv, debug_restore)
	return applied > 0
end

function is_syl_blank(syl)
	if syl.duration <= 0 then
		return true
	end

	-- try to remove common spacing characters
	local t = syl.text_stripped
	if t:len() <= 0 then return true end
	t = t:gsub("[ \t\n\r]", "") -- regular ASCII space characters
	t = t:gsub("　", "") -- fullwidth space
	return t:len() <= 0
end

function apply_one_syllable_template(syl, line, template, tenv, varctx, subs, skip_perchar, skip_multi)
	if aegisub.progress.is_cancelled() then return 0 end
	local t = template
	local applied = 0

	aegisub.debug.out(5, "Applying template to one syllable with text: %s\n", syl.text)

	-- Check for right inline_fx
	if t.fx and t.fx ~= syl.inline_fx then
		aegisub.debug.out(5, "Syllable has wrong inline-fx (wanted '%s', got '%s'), skipping.\n", t.fx, syl.inline_fx)
		return 0
	end

	if t.noblank and is_syl_blank(syl) then
		aegisub.debug.out(5, "Syllable is blank, skipping.\n")
		return 0
	end

	-- Recurse to per-char if required
	if not skip_perchar and t.perchar then
		aegisub.debug.out(5, "Doing per-character effects...\n")
		local charsyl = table.copy(syl)
		tenv.syl = charsyl

		local left, width = syl.left, 0
		local char_i = 0
		for c in unicode.chars(syl.text_stripped) do
			char_i = char_i + 1
			charsyl.text = c
			charsyl.text_stripped = c
			charsyl.text_spacestripped = c
			charsyl.prespace, charsyl.postspace = "", "" -- for whatever anyone might use these for
			width = aegisub.text_extents(syl.style, c)
			charsyl.left = left
			charsyl.center = left + width/2
			charsyl.right = left + width
			charsyl.prespacewidth, charsyl.postspacewidth = 0, 0 -- whatever...
			left = left + width
			set_ctx_syl(varctx, line, charsyl)

			local debug_restore = template_debug_enter(tenv, {
				phase = "apply-char",
				scope_kind = "char",
				char_index = char_i,
				char_text = c,
			})
			applied = applied + apply_one_syllable_template(charsyl, line, t, tenv, varctx, subs, true, false)
			template_debug_leave(tenv, debug_restore)
		end

		return applied
	end

	-- Recurse to multi-hl if required
	if not skip_multi and t.multi then
		aegisub.debug.out(5, "Doing multi-highlight effects...\n")
		local hlsyl = table.copy(syl)
		tenv.syl = hlsyl

		for hl = 1, syl.highlights.n do
			local hldata = syl.highlights[hl]
			hlsyl.start_time = hldata.start_time
			hlsyl.end_time = hldata.end_time
			hlsyl.duration = hldata.duration
			set_ctx_syl(varctx, line, hlsyl)

			local debug_restore = template_debug_enter(tenv, {
				phase = "apply-highlight",
				scope_kind = "highlight",
				highlight = hldata,
				highlight_index = hl,
				char_index = nil,
				char_text = nil,
			})
			applied = applied + apply_one_syllable_template(hlsyl, line, t, tenv, varctx, subs, true, true)
			template_debug_leave(tenv, debug_restore)
		end

		return applied
	end

	-- Regular processing
	if t.code then
		aegisub.debug.out(5, "Running code line\n")
		tenv.line = line
		local run_restore = template_debug_enter(tenv, {
			phase = "syl-code",
			scope_kind = tenv.syl and tenv.syl.isfuri and "furi" or "syl",
		})
		run_code_template(t, tenv)
		template_debug_leave(tenv, run_restore)
	else
		aegisub.debug.out(5, "Running %d effect loops\n", t.loops)
		for j, maxj in template_loop(tenv, t.loops) do
			local newline = table.copy(line)
			newline.styleref = syl.style
			newline.style = syl.style.name
			newline.layer = t.layer
			tenv.line = newline
			local run_restore = template_debug_enter(tenv, {
				phase = "syl-text",
				scope_kind = tenv.syl and tenv.syl.isfuri and "furi" or "syl",
			})
			newline.text = run_text_template(t.t, tenv, varctx, t)
			if t.keeptags then
				newline.text = newline.text .. syl.text
			elseif t.addtext then
				newline.text = newline.text .. syl.text_stripped
			end
			newline.effect = "fx"
			aegisub.debug.out(5, "Generated line with text: %s\n", newline.text)
			subs.append(newline)
			template_debug_record_generated_line(tenv, newline, {
				template_debug_id = t.debug_info and t.debug_info.template_debug_id or nil,
				template_kind = t.debug_info and t.debug_info.template_kind or nil,
				scope_kind = tenv.syl and tenv.syl.isfuri and "furi" or "syl",
				syl_i = tenv.syl and tenv.syl.i or nil,
				highlight_i = tenv.__aegi_template_debug and tenv.__aegi_template_debug.highlight_index or nil,
				char_i = tenv.__aegi_template_debug and tenv.__aegi_template_debug.char_index or nil,
			})
			update_template_debug_context("generated-line", t, tenv)
			applied = applied + 1
			template_debug_leave(tenv, run_restore)
		end
	end

	return applied
end


-- Main function to do the templating
function filter_apply_templates(subs, config)
	aegisub.progress.task("Collecting header data...")
	local meta, styles = karaskel.collect_head(subs, true)

	aegisub.progress.task("Parsing templates...")
	local templates = parse_templates(meta, styles, subs)

	aegisub.progress.task("Applying templates...")
	apply_templates(meta, styles, subs, templates)
end

function macro_apply_templates(subs, sel)
	filter_apply_templates(subs, {ismacro=true, sel=sel})
	aegisub.set_undo_point("apply karaoke template")
end

function macro_can_template(subs)
	-- check if this file has templates in it, don't allow running the macro if it hasn't
	local num_dia = 0
	for i = 1, #subs do
		local l = subs[i]
		if l.class == "dialogue" then
			num_dia = num_dia + 1
			-- test if the line is a template
			if (string.headtail(l.effect)):lower() == "template" then
				return true
			end
			-- don't try forever, this has to be fast
			if num_dia > 50 then
				return false
			end
		end
	end
	return false
end

aegisub.register_macro(tr"Apply karaoke template", tr"Applies karaoke effects from templates", macro_apply_templates, macro_can_template)
aegisub.register_filter(tr"Karaoke template", tr"Apply karaoke effect templates to the subtitles.\n\nSee the help file for information on how to use this.", 2000, filter_apply_templates)
