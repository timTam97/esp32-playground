import aws_cdk as cdk
from aws_cdk import (
    Stack,
    aws_lambda as lambda_,
    aws_sns as sns,
    aws_sns_subscriptions as subscriptions,
    aws_s3 as s3,
    aws_iam as iam,
    CfnOutput,
    Duration,
    RemovalPolicy,
)
from constructs import Construct


class GarageMonitorStack(Stack):
    def __init__(self, scope: Construct, id: str, **kwargs):
        super().__init__(scope, id, **kwargs)

        email = self.node.try_get_context("email")

        # SNS Topic for garage door alerts
        topic = sns.Topic(self, "GarageDoorAlerts", display_name="Garage Door Alerts")

        if email:
            topic.add_subscription(subscriptions.EmailSubscription(email))

        # S3 bucket for garage images
        bucket = s3.Bucket(
            self,
            "GarageImages",
            removal_policy=RemovalPolicy.RETAIN,
            lifecycle_rules=[
                s3.LifecycleRule(expiration=Duration.days(30)),
            ],
        )

        # Lambda function
        handler = lambda_.Function(
            self,
            "GarageMonitorHandler",
            runtime=lambda_.Runtime.PYTHON_3_12,
            code=lambda_.Code.from_asset("infra/lambda"),
            handler="handler.lambda_handler",
            timeout=Duration.seconds(30),
            memory_size=512,
            environment={
                "SNS_TOPIC_ARN": topic.topic_arn,
                "S3_BUCKET": bucket.bucket_name,
                "BEDROCK_MODEL_ID": "global.anthropic.claude-haiku-4-5-20251001-v1:0",
            },
        )

        # Permissions
        bucket.grant_put(handler)
        topic.grant_publish(handler)
        handler.add_to_role_policy(
            iam.PolicyStatement(
                actions=["bedrock:InvokeModel"],
                resources=["*"],
            )
        )

        # Function URL (no API Gateway needed)
        fn_url = handler.add_function_url(
            auth_type=lambda_.FunctionUrlAuthType.NONE,
        )

        CfnOutput(self, "FunctionUrl", value=fn_url.url)
        CfnOutput(self, "SnsTopicArn", value=topic.topic_arn)
        CfnOutput(self, "ImageBucket", value=bucket.bucket_name)
