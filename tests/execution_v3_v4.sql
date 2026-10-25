-- Use a dedicated database. --profile executes queries and prints actual counters.
drop table if exists v34_left;
drop table if exists v34_right;
drop table if exists v34_keys;
drop table if exists v34_float;
create table v34_left (id int, label char(16));
create table v34_right (id int, label char(16));
insert into v34_left values (1, 'alice'), (1, 'bob'), (2, 'carol');
insert into v34_right values (1, 'math'), (2, 'english');

-- 2 rows after filtering; the scan may incorporate the single-table condition.
select label, id from v34_left where id = 1;
-- 3 matched rows, 6 candidate combinations, right opens=3 and out_rows=6.
select l.label, r.label from v34_left l, v34_right r where l.id = r.id;
-- Aggregate consumes 3 rows during open and emits one row: count=3, sum=4, avg=4/3.
select count(*), sum(id), avg(id) from v34_left;
select id, count(*) from v34_left group by id;

create table v34_keys (a char(32), b char(32));
insert into v34_keys values ('a;4|b', 'c'), ('a', 'b;4|c'), ('a;4|b', 'c');
-- Two groups with counts 2 and 1; the old delimiter-based key merged them.
select a, b, count(*) from v34_keys group by a, b;

create table v34_float (tag int, v float);
insert into v34_float values (1, 1.0000001), (2, 1.0000002), (1, 1.0000001);
-- Two groups: min(tag)=1/count=2 and min(tag)=2/count=1.
-- Use tags because normal display still rounds these nearby float values.
select min(tag), count(*) from v34_float group by v;

delete from v34_left;
-- No NULL support: this project's scalar count/sum/avg over empty input return zeros.
select count(*), sum(id), avg(id) from v34_left;
-- Empty grouped input emits zero groups.
select id, count(*) from v34_left group by id;
