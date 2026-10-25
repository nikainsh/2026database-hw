-- minidb 端到端示例：覆盖存储管理器、缓冲管理器、查询处理器、执行引擎、用户界面
drop table if exists student;
drop table if exists teacher;

create table student (
  id int,
  name char(16),
  age int,
  score float,
  primary key (id)
);

create table teacher (
  id int,
  name char(16),
  dept char(8)
);

insert into student values (1, 'alice', 20, 88.5);
insert into student (id, name, age, score) values (2, 'bob', 22, 92.0), (3, 'carol', 19, 76.0);
insert into teacher values (1, 'dave', 'cs');
insert into teacher values (2, 'erin', 'math');

show tables;
desc student;

select * from student;
select id, name, age from student where age > 19;
select name from student where id = 2;
select s.name, t.name from student s, teacher t where s.id = t.id;

-- 建索引后相同查询应改走 index_scan（用 explain 验证）
create index idx_age on student (age);
explain select * from student where id = 3;
explain select * from student where age >= 20;
select name from student where age >= 20;

select count(*), sum(age), avg(score), min(name), max(name) from student;
select age, count(*) from student group by age;

update student set score = 95.5 where id = 1;
select id, score from student where id = 1;

delete from student where age < 20;
select count(*) from student;

-- 回归：非唯一索引上重复键跨越多个叶子时，范围扫描必须全部返回（B1）
-- k 用 char(1000) 让索引 order 变成 8，十来行数据就能把重复键打到多个叶子上
drop table if exists dupcheck;
create table dupcheck (id int, k char(1000));
create index dup_k on dupcheck (k);
insert into dupcheck values (1,'same'),(2,'same'),(3,'same'),(4,'same'),(5,'same'),(6,'same'),(7,'same'),(8,'same'),(9,'same'),(10,'same'),(11,'same'),(12,'same');
insert into dupcheck values (13,'other'),(14,'other');
select count(*) from dupcheck where k = 'same';

-- 回归：两个都建了索引的列各带一个范围条件时，键范围不能跨列串用（A1）
drop table if exists twocol;
create table twocol (id int, cat int);
create index tw_id on twocol (id);
create index tw_cat on twocol (cat);
insert into twocol values (0,0),(1,1),(2,2),(3,3),(4,0),(5,1),(6,2),(7,3);
select count(*) from twocol where id > 2 and cat < 4;

-- 回归：连接条件挂到 join 节点 + 按连接键的索引查找（P1b）
-- join_big.k 建索引前后各测一次，两种访问路径必须给出同样的答案
drop table if exists join_small;
drop table if exists join_big;
create table join_small (k int, tag char(8));
create table join_big (id int, k int);
insert into join_small values (1,'a'),(2,'b'),(3,'c'),(7,'d');
insert into join_big values (1,1),(2,1),(3,2),(4,2),(5,2),(6,3),(7,5),(8,7);
select count(*) from join_small, join_big where join_small.k = join_big.k;
select sum(join_big.id) from join_small, join_big where join_small.k = join_big.k;
create index jb_k on join_big (k);
select count(*) from join_small, join_big where join_small.k = join_big.k;
select sum(join_big.id) from join_small, join_big where join_small.k = join_big.k;
explain select count(*) from join_small, join_big where join_small.k = join_big.k;
select count(*) from join_big, join_small where join_big.k = join_small.k;
select count(*) from join_small, join_big where join_small.k = join_big.k and join_big.id > 2;
select count(*) from join_small, join_big;
