-- One step of the snake: turn, move, collide, eat. At most three cells
-- are drawn per step and nothing is allocated.

local function step()
    -- Take the next queued turn.
    if queued > 0 then
        dir = queue[1]
        queue[1], queue[2] = queue[2], queue[3]
        queued = queued - 1
    end
    local next_cell = head + STEP[dir]

    -- The tail moves first unless the snake is growing, so following
    -- your own tail into the cell it leaves is allowed. A body cell
    -- holds the direction of the next segment, which is where the tail
    -- goes.
    local vacated, vacated_value = 0, 0
    if grow > 0 then
        grow = grow - 1
    else
        vacated, vacated_value = tail, get(tail)
        set(tail, EMPTY)
        tail = tail + STEP[vacated_value - BODY + 1]
        length = length - 1
    end

    local what = get(next_cell)
    if what == WALL or what >= BODY then
        if vacated ~= 0 then          -- put the tail back as it was drawn
            set(vacated, vacated_value)
            tail = vacated
            length = length + 1
        end
        game_over()
        return
    end

    if vacated ~= 0 then
        draw_cell(vacated, 32, EMPTY_ATTR)
    end
    set(head, BODY + dir - 1)          -- the old head points at the new one
    draw_cell(head, 32, BODY_ATTR)
    head = next_cell
    set(head, BODY + dir - 1)
    length = length + 1
    draw_cell(head, 32, HEAD_ATTR)

    if what == FOOD then
        grow = grow + GROW_PER_FOOD
        eaten = eaten + 1
        add_score(FOOD_POINTS * level)
        Sound.Tone(880, 30)
        if eaten >= FOODS_PER_LEVEL then
            draw_hud()
            next_level()
            return
        end
        place_food()
        if bonus_steps == 0 and math.random() < BONUS_CHANCE then
            place_bonus()
        end
        update_speed()
        draw_hud()
    elseif what == BONUS then
        add_score(bonus_digit() * FOOD_POINTS * level)
        bonus_cell, bonus_steps = 0, 0     -- eaten: the head covers it
        Sound.Tone(1320, 60)
        draw_hud()
    else
        tick_bonus()
    end
end
