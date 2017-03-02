-- Tests for HASH index type

s3 = box.schema.space.create('my_space4')
i3_1 = s3:create_index('my_space4_idx1', {type='HASH', parts={1, 'scalar', 2, 'integer', 3, 'number'}, unique=true})
i3_2 = s3:create_index('my_space4_idx2', {type='HASH', parts={4, 'string', 5, 'scalar'}, unique=true})
s3:insert({100.5, 30, 95, "str1", 5})
s3:insert({"abc#$23", 1000, -21.542, "namesurname", 99})
s3:insert({true, -459, 4000, "foobar", "36.6"})
s3:select{}

i3_1:select({100.5})
i3_1:select({true, -459})
i3_1:select({"abc#$23", 1000, -21.542})

i3_2:select({"str1", 5})
i3_2:select({"str"})
i3_2:select({"str", 5})
i3_2:select({"foobar", "36.6"})

s3:drop()

-- #2112 int vs. double compare
s5 = box.schema.space.create('my_space5')
_ = s5:create_index('primary', {parts={1, 'scalar'}})
-- small range 1
s5:insert({5})
s5:insert({5.1})
s5:select()
s5:truncate()
-- small range 2
s5:insert({5.1})
s5:insert({5})
s5:select()
s5:truncate()
-- small range 3
s5:insert({-5})
s5:insert({-5.1})
s5:select()
s5:truncate()
-- small range 4
s5:insert({-5.1})
s5:insert({-5})
s5:select()
s5:truncate()
-- conversion to another type is lossy for both values
s5:insert({18446744073709551615ULL})
s5:insert({3.6893488147419103e+19})
s5:select()
s5:truncate()
-- insert in a different order to excersise another codepath
s5:insert({3.6893488147419103e+19})
s5:insert({18446744073709551615ULL})
s5:select()
s5:truncate()
-- MP_INT vs MP_UINT
s5:insert({-9223372036854775808LL})
s5:insert({-3.6893488147419103e+19})
s5:select()
s5:truncate()
-- insert in a different order to excersise another codepath
s5:insert({-3.6893488147419103e+19})
s5:insert({-9223372036854775808LL})
s5:select()
s5:truncate()
-- different signs 1
s5:insert({9223372036854775807LL})
s5:insert({-3.6893488147419103e+19})
s5:select()
s5:truncate()
-- different signs 2
s5:insert({-3.6893488147419103e+19})
s5:insert({9223372036854775807LL})
s5:select()
s5:truncate()
-- different signs 3
s5:insert({-9223372036854775808LL})
s5:insert({3.6893488147419103e+19})
s5:select()
s5:truncate()
-- different signs 4
s5:insert({3.6893488147419103e+19})
s5:insert({-9223372036854775808LL})
s5:select()
s5:truncate()
-- different magnitude 1
s5:insert({1.1})
s5:insert({18446744073709551615ULL})
s5:select()
s5:truncate()
-- different magnitude 2
s5:insert({18446744073709551615ULL})
s5:insert({1.1})
s5:select()
s5:truncate()

s5:drop()
