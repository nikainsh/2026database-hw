-- Use a dedicated demo database, e.g. --data ./build/v5_demo --db executor_v5.
drop table if exists hj_demo_left;
drop table if exists hj_demo_right;
create table hj_demo_left (id int, label char(16));
create table hj_demo_right (id int, score int);
insert into hj_demo_left values (1,'alice'),(1,'bob'),(2,'carol');
insert into hj_demo_right values (1,80),(1,90),(2,70),(9,99);
-- Hash Join: five rows; candidates=5, build_rows=3, probe_rows=4.
explain select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where l.id=r.id;
select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where l.id=r.id;
-- Preserve the additional filter: three rows (alice/bob 90, carol 70).
select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where r.id=l.id and r.score<>80;
-- OR alone cannot be used as a hash key: nested loop, seven rows.
explain select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where l.id=r.id or l.label='bob';
select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where l.id=r.id or l.label='bob';
-- Changes must be reflected in a fresh execution: three rows remain.
delete from hj_demo_left where label='alice';
select l.id,l.label,r.score from hj_demo_left l,hj_demo_right r where l.id=r.id;
