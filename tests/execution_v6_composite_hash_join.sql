-- Use --data ./build/v6_demo --db executor_v6 with --profile.
drop table if exists hj_class_left;
drop table if exists hj_class_right;
create table hj_class_left (class_id int, student_id int, label char(8));
create table hj_class_right (class_id int, student_id int, score int);
insert into hj_class_left values (1,1,'alice'),(1,2,'bob'),(1,2,'bob2'),(2,1,'carol');
insert into hj_class_right values (1,1,80),(1,2,90),(1,2,91),(2,1,70),(2,9,99);
-- One-key query: 11 rows/candidates (different semantics from the next query).
explain select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id;
select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id;
-- Joint key: 6 rows/candidates, key_count=2, duplicates preserved.
explain select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id and r.student_id=l.student_id;
select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id and r.student_id=l.student_id;
-- Other predicates still apply: 2 rows (bob scores 90 and 91).
select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id and l.student_id=r.student_id and r.score>80 and l.label<>'bob2';
-- OR student condition cannot become part of the hash key: key_count=1, 7 rows.
explain select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id and (l.student_id=r.student_id or r.student_id=9);
select l.label,r.score from hj_class_left l,hj_class_right r where l.class_id=r.class_id and (l.student_id=r.student_id or r.student_id=9);
