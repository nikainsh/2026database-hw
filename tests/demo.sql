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
