select count(*) from bench;
select count(*) from bench where cat >= 0;
select count(*) from bench where cat < 50;
select count(*) from bench where id > 2 and cat < 4;
select count(*) from bench, dim where bench.id = dim.k;
select count(*) from bench, dim where bench.cat = dim.g;
select cat, count(*) from bench where cat < 3 group by cat;
select count(*) from dupcheck where k = 'same';
