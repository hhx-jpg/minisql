use testdb;
select * from account where id = 12500000;
select * from account where balance = 514.35;
select * from account where name = "name56789";
select id, name from account where balance >= 500.00 and balance < 600.00;
select name, balance from account where balance > 300.00 and id <= 12500100;
select * from account where id < 12514550 and name > "name14500";
select * from account where id < 12500200 and name < "name00100";
insert into account values(12500000, "dup_test", 999.99);
create index idx01 on account(name);
select * from account where name = "name56789";
select * from account where name = "name45678";
select * from account where id < 12500200 and name < "name00100";
delete from account where name = "name45678";
insert into account values(12545678, "name45678", 456.78);
drop index idx01;
select * from account where name = "name56789";
update account set id = 99999999, balance = 8888.88 where name = "name56789";
select * from account where name = "name56789";
delete from account where balance = 514.35;
select * from account where id = 12500000;


