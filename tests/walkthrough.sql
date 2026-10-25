-- 基本操作走一遍：建库建表、插入、查询、索引、聚合、分组、改删、重启验证持久化。
-- 跑法见 docs/how-to-test.md。每条命令后面 CLI 会打印 (N us) 用时。

create table student (id int primary key, name char(16), age int, score float);
insert into student values (1,'alice',20,88.5),(2,'bob',22,91.0),(3,'carol',19,76.5),(4,'dave',21,95.5);

show tables;
desc student;

select * from student;
select name, score from student where age >= 21;
select * from student where name = 'bob';

explain select * from student where id = 3;

create index idx_age on student (age);
explain select * from student where age >= 20;
select name, age from student where age >= 20;

select count(*), sum(age), avg(score), min(score), max(score) from student;
select age, count(*) from student group by age;

update student set score = 99.0 where id = 1;
select id, name, score from student where id = 1;

delete from student where age < 20;
select count(*) from student;
