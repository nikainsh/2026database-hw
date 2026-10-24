-- Run only in a dedicated demonstration database (for example --db executor_v2).
drop table if exists v2_left;
drop table if exists v2_right;
create table v2_left (id int, label char(16));
create table v2_right (id int, label char(16), primary key(id));
insert into v2_right values (1, 'math'), (2, 'english');

-- Empty left + indexed right: expect 0 rows and no crash.
explain select l.id, r.label from v2_left l, v2_right r where r.id = 1;
select l.id, r.label from v2_left l, v2_right r where r.id = 1;

-- Duplicate left keys: expect (1, alice, math), (1, bob, math), (2, carol, english).
insert into v2_left values (1, 'alice'), (1, 'bob'), (2, 'carol');
select l.id, l.label, r.label from v2_left l, v2_right r where l.id = r.id;
select l.id, l.label, r.label from v2_left l, v2_right r where l.id = r.id;

-- Empty right: expect 0 rows.
delete from v2_right;
select l.id, r.label from v2_left l, v2_right r where l.id = r.id;
