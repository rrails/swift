$:.unshift(File.join(File.dirname(__FILE__), '..', 'lib'))

require 'bundler/setup'
require 'benchmark'
require 'stringio'
require 'swift'
require 'swift/adapter/mysql'
require 'swift/adapter/postgres'
require 'swift/adapter/sqlite3'
require 'swift/migrations'

class User < Swift::Record
  store     :users
  attribute :id,         Swift::Type::Integer, serial: true, key: true
  attribute :name,       Swift::Type::String
  attribute :email,      Swift::Type::String
  attribute :updated_at, Swift::Type::DateTime
end # User

class Runner
  attr_reader :tests, :driver, :runs, :rows
  def initialize opts = {}
    @driver = opts[:driver]
    klass = case @driver
      when /postgresql/ then Swift::Adapter::Postgres
      when /mysql/      then Swift::Adapter::Mysql
      when /sqlite3/    then Swift::Adapter::Sqlite3
    end

    %w(tests runs rows).each do |name|
      instance_variable_set("@#{name}", opts[name.to_sym])
    end

    Swift.setup :default, klass, db: @driver == 'sqlite3' ? ':memory:' : 'swift', ssl: {sslmode: 'disable'}
  end

  def run
    User.migrate!     if tests.include?(:create)
    yield run_creates if tests.include?(:create)
    yield run_selects if tests.include?(:select)
    yield run_updates if tests.include?(:update)
    yield run_writes  if tests.include?(:update)
  end

  def run_creates
    Benchmark.run('swift #create') do
      rows.times{|n| User.create(name: "test #{n}", email: "test@example.com", updated_at: Time.now)}
    end
  end

  def run_selects
    Benchmark.run('swift #select') do
      runs.times{ User.execute("select * from #{User}").each {|m| [m.id, m.name, m.email, m.updated_at] }}
    end
  end

  def run_updates
    Benchmark.run('swift #update') do
      runs.times do |n|
        User.execute("select * from #{User}").each do |m|
          m.update(name: 'foo', email: 'foo@example.com', updated_at: Time.now)
        end
      end
    end
  end

  def run_writes
    Swift.db.execute('truncate users') rescue Swift.db.execute('delete from users') # sqlite3
    Benchmark.run('swift #write') do
      stream = StringIO.new rows.times.map {|n| "test #{n}\ttest@example.com\t#{Time.now}\n" }.join('')
      Swift.db.write(User, %w{name email updated_at}, stream)
    end
  end
end
