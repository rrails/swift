# Extension.
require_relative '../ext/swift/dbi'
require 'delegate'
require 'pp'

module Swift
  class << self
    def model &definition
      Model.schema(&definition)
    end

    def setup name, adapter = {}
      name, adapter = :default, name unless name.kind_of?(Symbol)
      (@repositories ||= {})[name] = adapter.kind_of?(Adapter) ? adapter : Adapter.new(adapter)
    end

    def db name = :default, &block
      scope = @repositories[name] or raise "Unknown db '#{name}', did you forget to #setup?"
      scope.dup.instance_eval(&block) if block_given?
      scope
    end
  end

  class Statement < DBI::Statement
    def initialize adapter, model, query
      @model = model
      super adapter, query
    end

    def each
      super{|att| yield @model.load att}
    end
  end

  class Adapter < DBI::Handle
    def prepare model, query
      return super(model) unless model < Model
      Statement.new(self, model, query)
    end

    def get model, *ids
      keys = model.keys.map{|k| "#{k.field} = ?"}.join(', ')
      prepare(model, "select * from #{model.resource} where #{keys}").execute(*ids).first
    end

    def create *resources
    end

    def transaction name = nil, &block
      super(name){ self.dup.instance_eval(&block)}
    end
  end # Adapter

  class Property
    attr_accessor :name, :field, :type, :key
    alias_method :key?, :key

    def initialize name, type, options = {}
      @name, @type, @field, @key = name, type, options.fetch(:field, name), options.fetch(:key, false)
    end
  end # Property

  class Model
    alias_method :model, :class

    def properties by = :property
      return model.properties if by == :property
      model.properties.inject({}) do |ac, p|
       ac[p.send(by)] = instance_variable_get("@#{p.name}") unless instance_variable_get("@#{p.name}").nil?
       ac
      end
    end

    def self.load attributes
      obj = new
      model.names.zip(attrbitues.values_at(*model.fields)).each{|k, v| obj.instance_variable_set("@#{k}", v)}
      obj
    end

    def save
    end

    class << self
      attr_accessor :properties, :resource
      def fields;   @fields ||= properties.values.map(&:field) end
      def names;    @names  ||= properties.keys                end
      def key;      @key    ||= properties.values.find(&:key?) end

      def inherited klass
        klass.resource   ||= klass.to_s.downcase.gsub(/[^:]::/, '')
        klass.properties ||= {}
        klass.properties.update(properties || {})
      end

      def schema &definition
        Dsl.new(self, &definition).model
      end
    end
  end

  class Model::Dsl
    attr_reader :model

    def initialize model, &definition
      @model = Class.new(model)
      instance_eval(&definition)
    end

    def property name, type, options = {}
      @model.properties[name] = property = Property.new(name, type, options)
      (class << @model; self end).send(:define_method, name, lambda{ property})
      @model.send(:define_method, :name, lambda{ instance_variable_get(:"@#{name}")})
      @model.send(:define_method, :"#{name}=", lambda{|value| instance_variable_set(:"@#{name}", value)})
    end

    def resource name
      @model.resource = name
    end
  end
end

