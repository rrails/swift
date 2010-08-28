require_relative 'helper'
require 'stringio'

describe 'Adapter' do
  supported_by Swift::DB::Postgres, Swift::DB::Mysql do
    describe 'db' do
      it 'yields db to block' do
        Swift.db do |db|
          assert_kind_of Swift::Adapter, db
        end
      end
    end

    describe 'execute' do
      it 'executes without bind values' do
        assert Swift.db.execute %q{drop table if exists users}
      end

      it 'executes with bind values' do
        Swift.db.execute %q{drop table if exists users}
        Swift.db.execute %q{create table users(id serial, name text, created_at timestamp)}
        assert Swift.db.execute 'insert into users (name, created_at) values (?, now())', 'Benny Arthurton'
      end
    end

    describe 'prepared statements' do
      before do
        @db = Swift.db do |db|
          db.execute %q{drop table if exists users}
          db.execute %q{create table users(id serial, name text, created_at timestamp)}
        end
      end

      it 'executes without bind values' do
        assert @db.prepare(%q{insert into users (name, created_at) values ('Apple Arthurton', now())}).execute
      end

      it 'executes with bind values' do
        assert @db.prepare(%q{insert into users (name, created_at) values (?, now())}).execute('Apple Arthurton')
      end

      it 'executes multiple times' do
        sth = @db.prepare(%q{insert into users (name, created_at) values (?, now())})
        assert sth.execute('Apple Arthurton')
        assert sth.execute('Benny Arthurton')
      end
    end

    describe 'executed prepared statements' do
      before do
        @db = Swift.db do |db|
          db.execute %q{drop table if exists users}
          db.execute %q{create table users(id serial, name text, created_at timestamp)}
          sth = db.prepare(%q{insert into users (name, created_at) values (?, now())})
          sth.execute('Apple Arthurton')
          sth.execute('Benny Arthurton')
        end
        @sth = @db.prepare('select * from users').execute
      end

      it 'enumerates' do
        assert_kind_of Enumerable, @sth
      end

      it 'enumerates block' do
        begin
          @sth.execute{|row| row}
        rescue => error
          flunk error.message
        else
          pass
        end
      end

      it 'returns hash tuples for enumerable methods' do
        assert_kind_of Hash, @sth.first
      end

      it 'returns a result set on Adapter#execute{}' do
        @db.execute('select * from users') {|r| assert_kind_of Hash, r }
      end

      it 'returns a result set on Adapter#results' do
        @db.execute('select * from users')
        assert_kind_of Swift::Result, @db.results
      end
    end


    #--
    # TODO: Not sure how I feel about the block in write; feels like it's just there to get around the fields in the
    # argument list. How about write('users', %w{name, email, balance}, data)?
    describe 'bulk writes!' do
      before do
        @db = Swift.db do |db|
          db.execute %q{drop table if exists users}
          db.execute %q{create table users(id serial, name text, email text)}
        end
      end

      it 'writes from an IO object' do
        data = StringIO.new "Sally Arthurton\tsally@local\nJonas Arthurton\tjonas@local\n"
        assert_equal 2, Swift.db.write('users', %w{name email}, data)
      end

      it 'writes from a string' do
        data = "Sally Arthurton\tsally@local\nJonas Arthurton\tjonas@local\n"
        assert_equal 2, Swift.db.write('users', %w{name email}, data)
      end
    end
  end
end
